#!/bin/bash
# tiramisu/tools/build-test.sh
# Offline verification: build the guest PE (clang cross, no SDK needed),
# build the loader + host-side test on Linux, run it.
# Exit 0 = every check passed. This is the gate before any console work.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
out=tests/out
mkdir -p "$out"

# --- pick the LLVM 19 toolchain (STL/loader parity with the console build) ---
LLVM_BIN="/usr/lib/llvm-19/bin"
CLANG="$LLVM_BIN/clang"
LLD_LINK="$HOME/.local/bin/lld-link"
LLVM_DLLTOOL="$LLVM_BIN/llvm-dlltool"
[[ -x "$CLANG" ]] || CLANG="$HOME/.local/bin/clang-19"
[[ -x "$CLANG" ]] || { echo "ERROR: clang 19 not found" >&2; exit 1; }
"$CLANG" --version | head -1

# --- 1. import library for the guest (tiramisu_host.dll) ---
rm -f "$out/tiramisu_host.lib"
"$LLD_LINK" /lib /def:src/guest/tiramisu_host.def /out:"$out/tiramisu_host.lib" /machine:x64 /noentry

# --- 2. guest PE64: freestanding, imports bound at load time by trt ---
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_hello.c -o "$out/guest_hello.obj"
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_data.c -o "$out/guest_data.obj"
"$LLD_LINK" "$out/guest_hello.obj" "$out/guest_data.obj" "$out/tiramisu_host.lib" \
  /NODEFAULTLIB /ENTRY:guest_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /OUT:"$out/guest_hello.exe"
# internally-consistent low-base variant: fixups encode ImageBase 0x1A00000,
# the reloc-path test loads it off that base and checks the DIR64 pass
"$LLD_LINK" "$out/guest_hello.obj" "$out/guest_data.obj" "$out/tiramisu_host.lib" \
  /NODEFAULTLIB /ENTRY:guest_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_hello_reloc.exe"
file "$out/guest_hello.exe"

# --- 3. host-side loader test on Linux ---
clang++ -std=c++17 -O2 -Wall -no-pie \
  tests/test_pe_loader.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/vfs.cpp \
  src/trt/apiset.cpp src/trt/thunk.cpp src/trt/images.cpp src/trt/pe_exports.cpp src/trt/bridge_d3d.cpp src/trt/bridge_reg.cpp \
  src/trt/bridge_mem.cpp src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp src/trt/input.cpp \
  src/trt/bridge_kernel32.cpp \
  -o "$out/test_pe_loader"

"$out/test_pe_loader" "$out/guest_hello.exe" "$out/guest_hello_reloc.exe"

# --- 4. input bridge test (user32 surface the guests consume) ---
clang++ -std=c++17 -O2 -Wall -no-pie \
  tests/test_input.cpp \
  src/trt/input.cpp src/trt/bridge_user32.cpp src/trt/bridge_kernel32.cpp \
  src/trt/module_table.cpp src/trt/vfs.cpp src/trt/pe_loader.cpp \
  src/trt/apiset.cpp src/trt/thunk.cpp src/trt/images.cpp src/trt/pe_exports.cpp src/trt/bridge_d3d.cpp src/trt/bridge_reg.cpp \
  src/trt/bridge_mem.cpp src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp \
  -o "$out/test_input"
"$out/test_input"

# --- 5. THE launch-path proof: game-shaped PE through the full pipeline ---
rm -f "$out/kernel32.lib"
"$LLD_LINK" /lib /def:src/guest/kernel32.def /out:"$out/kernel32.lib" /machine:x64 /noentry
rm -f "$out/user32.lib"
"$LLD_LINK" /lib /def:src/guest/user32.def /out:"$out/user32.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_game.c -o "$out/guest_game.obj"
"$LLD_LINK" "$out/guest_game.obj" "$out/kernel32.lib" "$out/user32.lib" \
  /NODEFAULTLIB /ENTRY:game_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_game.exe"
clang++ -std=c++17 -O2 -Wall -no-pie \
  tests/test_game_boot.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/vfs.cpp \
  src/trt/apiset.cpp src/trt/thunk.cpp src/trt/images.cpp src/trt/pe_exports.cpp src/trt/bridge_d3d.cpp src/trt/bridge_reg.cpp \
  src/trt/bridge_mem.cpp src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp src/trt/input.cpp \
  src/trt/bridge_kernel32.cpp src/trt/bridge_user32.cpp \
  -o "$out/test_game_boot"
"$out/test_game_boot" "$out/guest_game.exe"

# --- 6. real-game load path: api-sets, delay imports, fail-visible stubs ---
# A guest that imports contract names (api-ms-win-*), a module the console
# does not have (advapi32) and a DELAY-loaded DLL (d3dcompiler_47).
rm -f "$out/api-ms-win-core-synch-l1-2-0.lib"
"$LLD_LINK" /lib /def:src/guest/api-ms-win-core-synch-l1-2-0.def \
  /out:"$out/api-ms-win-core-synch-l1-2-0.lib" /machine:x64 /noentry
rm -f "$out/api-ms-win-core-winrt-l1-1-0.lib"
"$LLD_LINK" /lib /def:src/guest/api-ms-win-core-winrt-l1-1-0.def \
  /out:"$out/api-ms-win-core-winrt-l1-1-0.lib" /machine:x64 /noentry
rm -f "$out/advapi32.lib"
"$LLD_LINK" /lib /def:src/guest/advapi32.def /out:"$out/advapi32.lib" /machine:x64 /noentry
rm -f "$out/d3dcompiler_47.lib"
"$LLD_LINK" /lib /def:src/guest/d3dcompiler_47.def /out:"$out/d3dcompiler_47.lib" /machine:x64 /noentry
rm -f "$out/delayimp.lib"
"$LLD_LINK" /lib /def:src/guest/delayimp.def /out:"$out/delayimp.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_realload.c -o "$out/guest_realload.obj"
"$LLD_LINK" "$out/guest_realload.obj" \
  "$out/api-ms-win-core-synch-l1-2-0.lib" "$out/api-ms-win-core-winrt-l1-1-0.lib" \
  "$out/advapi32.lib" "$out/d3dcompiler_47.lib" "$out/delayimp.lib" \
  /NODEFAULTLIB /ENTRY:real_load_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /DELAYLOAD:d3dcompiler_47.dll \
  /OUT:"$out/guest_realload.exe"
clang++ -std=c++17 -O2 -Wall -no-pie \
  tests/test_real_load.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/apiset.cpp \
  src/trt/thunk.cpp src/trt/vfs.cpp src/trt/images.cpp src/trt/pe_exports.cpp src/trt/bridge_d3d.cpp src/trt/bridge_reg.cpp \
  src/trt/bridge_mem.cpp src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp src/trt/input.cpp \
  src/trt/bridge_kernel32.cpp src/trt/bridge_user32.cpp \
  -o "$out/test_real_load"
"$out/test_real_load" "$out/guest_realload.exe"

# --- 7. guest DLL loading: the DLLs a game ships with it ---
cat > "$out/guestdll.def" <<'DEOF'
LIBRARY guestdll
EXPORTS
    dll_add
DEOF
rm -f "$out/guestdll.lib"
"$LLD_LINK" /lib /def:"$out/guestdll.def" /out:"$out/guestdll.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_dll.c -o "$out/guest_dll.obj"
# NOTE: /def on a DLL link makes lld write an import library next to the
# output - name it apart so it cannot clobber the one the exe links against
"$LLD_LINK" "$out/guest_dll.obj" "$out/tiramisu_host.lib" \
  /NODEFAULTLIB /ENTRY:dll_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1B00000 /DEF:"$out/guestdll.def" \
  /implib:"$out/guestdll.dll.lib" /OUT:"$out/guestdll.dll"
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_libload.c -o "$out/guest_libload.obj"
"$LLD_LINK" "$out/guest_libload.obj" "$out/guestdll.lib" "$out/kernel32.lib" \
  /NODEFAULTLIB /ENTRY:libload_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_libload.exe"
clang++ -std=c++17 -O2 -Wall -no-pie \
  tests/test_libload.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/apiset.cpp \
  src/trt/thunk.cpp src/trt/images.cpp src/trt/pe_exports.cpp src/trt/bridge_d3d.cpp src/trt/bridge_reg.cpp \
  src/trt/bridge_mem.cpp src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp src/trt/input.cpp \
  src/trt/vfs.cpp src/trt/bridge_kernel32.cpp src/trt/bridge_user32.cpp \
  -o "$out/test_libload"
"$out/test_libload" "$out/guest_libload.exe" "$out"

# --- 8. the graphics bridge: the guest's D3D11 calls must reach the host's GPU ---
# The guest is compiled against the REAL SDK headers (so the vtable slots are
# the SDK's), the host's GPU is faked in the test.
SDK="$HOME/.cache/uwp-crossbuild/xwin/sdk/include"
XCRT="$HOME/.cache/uwp-crossbuild/xwin/crt/include"
if [[ -d "$SDK/um" && -d "$XCRT" ]]; then
cat > "$out/d3d11.def" <<'DEOF'
LIBRARY d3d11
EXPORTS
    D3D11CreateDevice
DEOF
cat > "$out/dxgi.def" <<'DEOF'
LIBRARY dxgi
EXPORTS
    CreateDXGIFactory1
    CreateDXGIFactory2
    ?CreateDXGIFactory2@@YAJAEBU_GUID@@PEAPEAX@Z
DEOF
rm -f "$out/d3d11.lib" "$out/dxgi.lib"
"$LLD_LINK" /lib /def:"$out/d3d11.def" /out:"$out/d3d11.lib" /machine:x64 /noentry
"$LLD_LINK" /lib /def:"$out/dxgi.def" /out:"$out/dxgi.lib" /machine:x64 /noentry
"$CLANG" -x c++ --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -fno-exceptions -fno-rtti \
  -I"$SDK/um" -I"$SDK/shared" -I"$SDK/ucrt" -I"$XCRT" \
  -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS \
  -c src/guest/guest_d3d.c -o "$out/guest_d3d.obj"
"$LLD_LINK" "$out/guest_d3d.obj" "$out/d3d11.lib" "$out/dxgi.lib" \
  /NODEFAULTLIB /ENTRY:d3d_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_d3d.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_d3d.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/apiset.cpp \
  src/trt/thunk.cpp src/trt/images.cpp src/trt/pe_exports.cpp \
  src/trt/bridge_d3d.cpp src/trt/bridge_reg.cpp \
  src/trt/bridge_mem.cpp src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp src/trt/input.cpp \
  src/trt/vfs.cpp \
  src/trt/bridge_kernel32.cpp \
  src/trt/bridge_user32.cpp \
  -o "$out/test_d3d"
"$out/test_d3d" "$out/guest_d3d.exe"
else
  echo "SKIP graphics bridge suite: no Windows SDK at $SDK"
fi

# --- 9. the registry: UWP has none, games expect one ---
cat > "$out/advapi32.def" <<'DEOF'
LIBRARY advapi32
EXPORTS
    RegOpenKeyExW
    RegCreateKeyExW
    RegQueryValueExW
    RegSetValueExW
    RegCloseKey
    RegEnumKeyExW
DEOF
rm -f "$out/advapi32.lib"
"$LLD_LINK" /lib /def:"$out/advapi32.def" /out:"$out/advapi32.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_reg.c -o "$out/guest_reg.obj"
"$LLD_LINK" "$out/guest_reg.obj" "$out/advapi32.lib" \
  /NODEFAULTLIB /ENTRY:reg_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_reg.exe"
clang++ -std=c++17 -O2 -Wall -no-pie \
  tests/test_reg.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/apiset.cpp \
  src/trt/thunk.cpp src/trt/images.cpp src/trt/pe_exports.cpp \
  src/trt/bridge_d3d.cpp src/trt/bridge_reg.cpp \
  src/trt/bridge_mem.cpp src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp src/trt/input.cpp \
  src/trt/vfs.cpp \
  src/trt/bridge_kernel32.cpp src/trt/bridge_user32.cpp \
  -o "$out/test_reg"
DRIEC="$out/drive_c"; rm -rf "$DRIEC"; mkdir -p "$DRIEC"
"$out/test_reg" "$out/guest_reg.exe" "$DRIEC"

# --- 10. the engine-API batch: mapping, threads, system info, lifecycle ---
cat > "$out/engine.def" <<'DEOF'
LIBRARY kernel32
EXPORTS
    CreateFileMappingW
    MapViewOfFile
    UnmapViewOfFile
    CreateThread
    WaitForSingleObject
    CreateEventW
    SetEvent
    ResetEvent
    InitializeCriticalSection
    EnterCriticalSection
    LeaveCriticalSection
    GetVersionExW
    GetSystemInfo
    MultiByteToWideChar
    WideCharToMultiByte
    GetTempPathW
    GetTickCount64
DEOF
cat > "$out/engine_u.def" <<'DEOF'
LIBRARY user32
EXPORTS
    PostQuitMessage
    SetFocus
    GetFocus
    GetClientRect
DEOF
rm -f "$out/engine.lib" "$out/engine_u.lib"
"$LLD_LINK" /lib /def:"$out/engine.def" /out:"$out/engine.lib" /machine:x64 /noentry
"$LLD_LINK" /lib /def:"$out/engine_u.def" /out:"$out/engine_u.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_engine.c -o "$out/guest_engine.obj"
"$LLD_LINK" "$out/guest_engine.obj" "$out/engine.lib" "$out/engine_u.lib" \
  "$out/kernel32.lib" "$out/user32.lib" \
  /NODEFAULTLIB /ENTRY:engine_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_engine.exe"
clang++ -std=c++17 -O2 -Wall -no-pie \
  tests/test_engine.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/apiset.cpp \
  src/trt/thunk.cpp src/trt/images.cpp src/trt/pe_exports.cpp \
  src/trt/bridge_d3d.cpp src/trt/bridge_reg.cpp src/trt/bridge_mem.cpp \
  src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp src/trt/vfs.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp \
  src/trt/bridge_kernel32.cpp src/trt/bridge_user32.cpp src/trt/input.cpp \
  -o "$out/test_engine"
DRIEC="$out/drive_c"; rm -rf "$DRIEC"; mkdir -p "$DRIEC"
"$out/test_engine" "$out/guest_engine.exe" "$DRIEC"

# --- 9. DirectInput 8: factory -> device -> acquire -> poll -> state ---
cat > "$out/dinput.def" <<'DEOF'
LIBRARY dinput8
EXPORTS
    DirectInput8Create
    trt_di_feed_move
    trt_di_feed_button
    trt_di_feed_key
DEOF
rm -f "$out/dinput.lib"
"$LLD_LINK" /lib /def:"$out/dinput.def" /out:"$out/dinput.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_dinput.c -o "$out/guest_dinput.obj"
"$LLD_LINK" "$out/guest_dinput.obj" "$out/dinput.lib" \
  "$out/kernel32.lib" \
  /NODEFAULTLIB /ENTRY:dinput_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1B00000 /OUT:"$out/guest_dinput.exe"
clang++ -std=c++17 -O2 -Wall -no-pie \
  tests/test_dinput.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/apiset.cpp \
  src/trt/thunk.cpp src/trt/images.cpp src/trt/pe_exports.cpp \
  src/trt/bridge_d3d.cpp src/trt/bridge_reg.cpp src/trt/bridge_mem.cpp \
  src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp \
  src/trt/vfs.cpp src/trt/bridge_kernel32.cpp src/trt/bridge_user32.cpp \
  src/trt/input.cpp \
  -o "$out/test_dinput"
"$out/test_dinput" "$out/guest_dinput.exe"

# --- 10. XAudio2: engine -> voices -> PCM submit -> queue state ---
cat > "$out/audio.def" <<'DEOF'
LIBRARY xaudio2_9
EXPORTS
    XAudio2Create
    trt_audio_note_submit
    trt_audio_note_start
DEOF
rm -f "$out/audio.lib"
"$LLD_LINK" /lib /def:"$out/audio.def" /out:"$out/audio.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_audio.c -o "$out/guest_audio.obj"
"$LLD_LINK" "$out/guest_audio.obj" "$out/audio.lib" \
  /NODEFAULTLIB /ENTRY:audio_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1C00000 /OUT:"$out/guest_audio.exe"
clang++ -std=c++17 -O2 -Wall -no-pie \
  tests/test_audio.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/apiset.cpp \
  src/trt/thunk.cpp src/trt/images.cpp src/trt/pe_exports.cpp \
  src/trt/bridge_d3d.cpp src/trt/bridge_reg.cpp src/trt/bridge_mem.cpp \
  src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp src/trt/bridge_dinput.cpp \
  src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp \
  src/trt/vfs.cpp src/trt/bridge_kernel32.cpp src/trt/bridge_user32.cpp \
  src/trt/input.cpp \
  -o "$out/test_audio"
"$out/test_audio" "$out/guest_audio.exe"

# --- 11. Steamworks: the auth bridge (no DRM bypass, ever) ---
cat > "$out/steam.def" <<'DEOF'
LIBRARY steam_api64
EXPORTS
    SteamAPI_RestartAppIfNecessary
    SteamAPI_Init
    SteamAPI_Shutdown
    SteamAPI_IsSteamRunning
    SteamAPI_GetSteamID
    SteamAPI_SetRichPresence
    SteamAPI_RunCallbacks
DEOF
rm -f "$out/steam.lib"
"$LLD_LINK" /lib /def:"$out/steam.def" /out:"$out/steam.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_steam.c -o "$out/guest_steam.obj"
"$LLD_LINK" "$out/guest_steam.obj" "$out/steam.lib" \
  /NODEFAULTLIB /ENTRY:steam_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_steam.exe"
clang++ -std=c++17 -O2 -Wall -no-pie \
  tests/test_steam.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/apiset.cpp \
  src/trt/thunk.cpp src/trt/images.cpp src/trt/pe_exports.cpp \
  src/trt/bridge_d3d.cpp src/trt/bridge_reg.cpp src/trt/bridge_mem.cpp \
  src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp \
  src/trt/vfs.cpp src/trt/bridge_kernel32.cpp src/trt/bridge_user32.cpp \
  src/trt/input.cpp \
  -o "$out/test_steam"
"$out/test_steam" "$out/guest_steam.exe"

# --- 12. winmm timing: the clock games schedule their frames on -------------
rm -f "$out/winmm.lib"
"$LLD_LINK" /lib /def:src/guest/winmm.def /out:"$out/winmm.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_winmm.c -o "$out/guest_winmm.obj"
"$LLD_LINK" "$out/guest_winmm.obj" "$out/winmm.lib" \
  /NODEFAULTLIB /ENTRY:guest_winmm_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_winmm.exe"
clang++ -std=c++17 -O2 -Wall -no-pie \
  tests/test_winmm.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/apiset.cpp \
  src/trt/thunk.cpp src/trt/images.cpp src/trt/pe_exports.cpp \
  src/trt/bridge_d3d.cpp src/trt/bridge_reg.cpp src/trt/bridge_mem.cpp \
  src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp \
  src/trt/vfs.cpp src/trt/bridge_kernel32.cpp src/trt/bridge_user32.cpp \
  src/trt/input.cpp \
  -o "$out/test_winmm"
"$out/test_winmm" "$out/guest_winmm.exe"
