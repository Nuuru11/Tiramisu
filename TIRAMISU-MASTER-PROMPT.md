# TIRAMISU MASTER PROMPT — Universal PC Game Runtime for Xbox

Paste this whole document as the opening message for Gemini / AGY working in
`~/tiramisu`. It is the contract and the complete work map. Work inside the
repo; do not fork the plan.

---

You are the lead engineer on **Tiramisu** (`/home/jewboy420/tiramisu`): a signed
UWP "Game" app for Xbox Series X|S **Dev Mode** that loads and runs real Windows
x64 game EXEs in-process. The console is x64 (AMD Zen 2 / RDNA 2); guests run at
native speed, same process. No CPU emulation. Your mission: **boot any 64-bit
D3D11 or D3D12 PC game to playable on the TV.**

## End goal, in priority order

1. **A real commercial Steam/PC game boots to gameplay on the TV** (video, audio,
   input, save, quit) — with `tiramisu.log` + TV screenshot evidence.
2. **D3D11 games** (Unity Mono/IL2CPP, Godot, older engines) — all D3D11 titles
   that use standard Windows APIs.
3. **D3D12 heavy hitters** (Cyberpunk 2077, God of War, Spider-Man Remastered,
   RDR2, Elden Ring, Black Myth: Wukong) — D3D12 titles up to playable.
4. **Legacy + 32-bit** — D3D9/D3D8, PE32 (x86-32) via interpreter.
5. **Broad universality** — any game we own boots to playable.

## Non-negotiable rules

- **Evidence, not announcements.** A layer is "done" only when a test or a
  console run proves it. Never upgrade a claim without an artifact.
- **Fail-visible, never silent.** A missing import/feature names itself in the
  log (`dll!symbol`, HRESULT, last-error). STUB hits are work items, not success.
- **No DRM bypass, ever.** Steam layer is auth-only; `BIsSubscribedApp` answers 0
  visibly. Legitimate paths only: games we own, GOG/itch DRM-free, or Steam
  builds running without the client (`steam_appid.txt`).
- **No game files or credentials committed.** Real games live in `corpus-local/`
  (gitignored).
- **Offline tests green before console work.** Every fix lands with a test in
  `tests/` that would have caught the bug.
- **Small commits, one gate per commit.** Format: `v0.XX: what — measured result`.
- **Solo gates.** `tools/build-test.sh` etc. run ONE at a time. Check for live
  writers first (`find ~/tiramisu -type f -mmin -15 -not -path './.git/*'`).
- **gdb discipline:** never `gdb -batch -ex "commands 1"` — use `gdb -batch -x script.gdb`.
- **Kill every Nemu process after each test.** Never leave them on CPU.

## Environment (Linux cross-build)

```sh
cd ~/tiramisu
tools/build-test.sh     # offline gate: loader + all bridge tests
tools/corpus-smoke.sh   # load matrix over corpus-local/ (real PEs)
tools/corpus-run.sh     # run-depth: DllMain on DLLs, EXE boot ladder
tools/build-host.sh     # -> out/layout/tiramisu.exe
tools/pack.sh           # makemsix + sign -> out/Tiramisu.msix
tools/deploy.sh install out/Tiramisu.msix out/dev.cer   # DP_HOST=<ip> DP_PASS=<pass>
tools/deploy.sh launch <FamilyName>!App <FullName>
tools/deploy.sh screenshot tv.png    # ALWAYS look at the TV after console work
```

Toolchain: LLVM 19 (`clang-cl`, `lld-link`), xwin splat at `~/.cache/uwp-crossbuild/xwin`.
Console: Dev Mode, **Device Portal ON**, "Treat UWP apps as games by default" ON
(5 GB not 1 GB). Log lands in LocalState `tiramisu.log`; pull via Device Portal.

---

## Where the project actually is (ground truth, 2026-11-15)

### What works (proven offline):

| Layer | Status | Evidence |
|---|---|---|
| **PE64 loader** | ✅ Full | map/relocate/import-bind/TLS/W^X seal/unwind. 921 lines. |
| **kernel32 core** | ✅ 490+ fns registered | VirtualAlloc, VirtualFree, CreateProcessW, ReadFile, WriteFile, LoadLibrary, CreateThread, CreateEvent, CreateMutex, CreateSemaphore, CloseHandle, GetTickCount64, QueryPerformanceCounter, Sleep, FlushInstructionCache, CreateFileW, SetEvent, etc. |
| **kernel32 gap** | ✅ 156+ fns | InterlockedCompareExchange, InterlockedExchange, CopyFileW, DeleteFileW, CreatePipe, CreateNamedPipe, DeviceIoControl, CreateWaitableTimer, etc. |
| **ntdll** | ✅ NtAllocateVirtualMemory, NtFreeVirtualMemory, NtQueryVirtualMemory | |
| **user32 pump** | ✅ Full | GetMessage, PeekMessage, DispatchMessage, CreateWindowExW, ShowWindow, SetWindowText, all DPI/monitor/display APIs, raw input, touch/pointer, clipboard, window management. ~700 functions. |
| **user32 gap** | ✅ Additional | MsgWaitForMultipleObjects, SetWinEventHook, EnumWindows, RegisterClassExW, etc. |
| **D3D11/DXGI bridge** | ✅ Pass-through | Guest gets HOST's ID3D11Device* + HOST CoreWindow swapchain. IDXGIFactory shim returns host swapchain. `D3D11CreateDevice` returns real host device pointer. ID3D11Device methods NOT implemented because they call the real host device directly. |
| **D3D9/D3D8→D3D11** | ✅ Hook interceptor | D3D9 hooking backend (`d3d9_backend.hpp`, 70KB) intercepts D3D9 calls, renders via host D3D11 swapchain. |
| **D3DX9/10/11** | ✅ Present | d3dx9_30-43, d3dx10_33-43, d3dx11_43/47. |
| **D3D12 device** | ⚠️ 51/90 vtbl | CreateCommandQueue, CreateCommandAllocator, CreateGraphicsPipelineState, CreateCommandList, CreateDescriptorHeap, CreateRootSignature, CreateCommittedResource, CreateFence, CreateCommandList1. 39 methods are NOP/unimpl/ok stubs. **QUEUE VTBL LAYOUT IS WRONG — see BUG-1.** |
| **XAudio2 audio** | ✅ Full | IXAudio2 + IXAudio2SourceVoice COM, WASAPI sink. |
| **WASAPI** | ✅ Full COM | IAudioClient, IMMDevice, avrt.dll bridge. Real PCM rendering. |
| **DirectSound/OpenAL** | ✅ Present | |
| **COM/OLE** | ✅ Full | CoInitializeEx, CoCreateInstance, CoTaskMem, DCOM class factory, variant/BSTR/safearray. |
| **VFS** | ✅ C:/Z: | C: = LocalState overlay, Z: = game dir. CreateFileW/ReadFile/WriteFile with game-dir sibling DLL search. |
| **C++ EH unwinder** | ✅ Full | `.pdata` walk, RtlVirtualUnwind, RtlUnwindEx, VEH pass-1, funclet phase-2 unwind, boundary escape swallow. Proven with RACCOIN. |
| **GetWriteWatch** | ✅ Dual-surface | Console passthrough / Linux soft-dirty probe via /proc/self/pagemap. |
| **DispatcherQueue** | ✅ DONE | DQC/DQ factories + QI in bridge_winrt.cpp. |
| **IOCP/OVERLAPPED** | ✅ Present | CreateIoCompletionPort, GetQueuedCompletionStatus, PostQueuedCompletionStatus, GetOverlappedResult/Ex. |
| **Thread pool** | ✅ Present | CreateThreadpool*, QueueUserWorkItem, SetThreadpoolTimer, SetThreadpoolWait, etc. |
| **Thread sync** | ✅ Present | WaitForSingleObject, WaitForMultipleObjects, SleepEx, SRWLock, ConditionVariable, InitializeCriticalSection, Enter/Leave/Delete, TryEnter, Event/Mutex/Semaphore, PulseEvent, Set/Reset/SetEvent, TlsAlloc/SetValue/Free, Fiber conversion. |
| **Address sync** | ✅ Mostly | WaitOnAddress, WakeByAddressSingle, WakeByAddressAll. (WakeAddress missing — minor) |
| **Interlocked** | ✅ In msvcrt | InterlockedCompareExchange, InterlockedExchange. (Increment/Decrement/Add missing from kernel32 table.) |
| **Memory copy** | ✅ Scattered | RtlCopyMemory, RtlMoveMemory, RtlZeroMemory in d3dx9/d3d bridges. (RtlFillMemory missing from table.) |
| **Media Foundation** | ✅ Full | MFStartup, IMFSourceReader, MFCreateAttributes/MediaType/MemoryBuffer/Sample, IMFDXGIDeviceManager. |
| **Vulkan** | ✅ Present | vkCreateInstance → SPIR-V→HLSL→D3D11 translation. |
| **Steam auth** | ✅ Auth-only | steam_api64, steam_api bridges. |
| **GDI** | ✅ 48+ fns | CreateDC, CreateFont, SelectObject, CreatePen/Brush, GetDeviceCaps, GetTextMetrics, SetPixel, etc. |
| **Shell/SHLWAPI** | ✅ Present | SHGetFolderPath, KnownFolderPath, Path* helpers. |
| **Registry** | ✅ Present | UWP registry hive. |
| **System info** | ✅ Present | GetSystemInfo, GetNativeSystemInfo, GetEnvironmentVariableW, GetComputerNameW, etc. (in bridge_sys.cpp, some not registered in kernel32 table.) |
| **W^X two-phase JIT** | ✅ Present | VirtualAlloc returns RW; VirtualProtect flips to RX on FlushInstructionCache. |
| **OpenGL→D3D11** | ✅ Present | 156KB, 3997 lines, 180+ GL/WGL procs. GLSL→HLSL via glsl_hlsl. |
| **D3D12 host path** | ✅ Host creates real device | `host_d3d12_create_device` calls real `D3D12CreateDevice`. `acquire_d3d12_swapchain` callback exists. |
| **Host render** | ✅ D3D11 | `swapchain_create()` creates real D3D11 device + swapchain on CoreWindow. `swapchain_present()` calls `g_swap->Present(1,0)`. D3D9/GL/Vulkan backends exist for display routing. |
| **GDI (detailed)** | ✅ Partial | 48 functions registered. Missing: GetDC, ReleaseDC, BitBlt, StretchBlt, CreateCompatibleDC, GetStockObject. |
| **DirectXMath** | ✅ Present | bridge_sys_extras.cpp registers directxmath.dll. |

### What boots (proven offline):

| Game | Engine | Graphics | Depth Ladder | Status |
|---|---|---|---|---|
| **RACCOIN.exe** | Unity IL2CPP (Mono) | D3D11 | 7/7 | BOOTED: CRT-STARTUP→WINMAIN→UNITYMAIN2→WINDOW→D3D11→PUMP→QUIT. (ehfix19, naked-asm IAT probe for UnityMain2.) |
| **Go Next demo.exe** | Unity IL2CPP (Mono) | D3D11 | 7/7 | BOOTED: UNITYMAIN2 via IAT probe. |
| **OrTMM.exe** | PyInstaller/Python 3.13 | N/A (launcher) | Partial | CRT→WINMAIN→WINDOW→PUMP→QUIT. ExitProcess(259). Python spawn child died signal 11. |
| **GoWLauncher.exe** | Proprietary | D3D12 | Partial | Loaded 1/0 (launcher only, not the game). |

### What DOES NOT boot (verified from latest corpus-run):

- **RACCOIN signal 11** in latest run — confirmed race condition with live writer
  (`tests/test_settings.cpp`). The gate was green when the other session settled.
- **DLLs all RUN OK** — 8/8 corpus DLLs load and run DllMain successfully.
- **D3D12 games** — no D3D12 game has been tested because the command list recording
  methods are not implemented.
- **Zero games on Xbox** — the project has never been deployed to Dev Mode TV.

---

## 🔴 CRITICAL BUGS FOUND (2026-11-15 deep dive — fix these FIRST)

### BUG-1: D3D12 CommandQueue vtbl LAYOUT IS WRONG (blocks every D3D12 game)

`g_queue_vtbl[19]` in `bridge_d3d12.cpp` has **19 entries when ID3D12CommandQueue
has only 13 methods (slots 0-12)**. Worse, the methods are at the WRONG SLOTS:

**Real ID3D12CommandQueue ABI (what games expect):**
```
Slot 0-2:   IUnknown (QueryInterface, AddRef, Release)
Slot 3-6:   ID3D12Object (GetPrivateData, SetPrivateData, SetPrivateDataInterface, SetName)
Slot 7:     ID3D12Pageable::GetDevice
Slot 8:     ExecuteCommandLists   ← THE critical method
Slot 9:     Signal
Slot 10:    Wait
Slot 11:    GetTimestampFrequency
Slot 12:    GetClockCalibration
```

**Our vtbl (current, WRONG):**
```
Slot 8:  queue_update_tile   ← no-op! Game's ExecuteCommandLists call does NOTHING
Slot 9:  queue_copy_tile     ← no-op! Game's Signal call does NOTHING
Slot 10: queue_exec_lists   ← only increments counter! Game's Wait call does NOTHING
Slot 11: queue_marker_evt
Slot 12: queue_marker_evt
Slot 13: queue_end_evt
Slot 14: queue_signal       ← wrong slot!
Slot 15: queue_wait         ← wrong slot!
Slot 16: queue_ts_freq      ← wrong slot!
Slot 17: queue_clock_cal    ← wrong slot!
Slot 18: queue_get_desc
```

**Impact:** Every D3D12 game that calls `ExecuteCommandLists` (i.e. every D3D12
game that renders anything) silently does nothing. They don't crash — they
just never render. This bug is why no D3D12 game has ever reached a rendered
frame.

**Fix (~50 lines):**
```cpp
const void* g_queue_vtbl[13] = {
    qi, addref, release,
    child_get_priv, child_set_priv, child_set_priv_iface, child_set_name,
    child_get_device,
    queue_exec_lists,  // slot 8: ExecuteCommandLists
    queue_signal,      // slot 9: Signal
    queue_wait,        // slot 10: Wait
    queue_ts_freq,     // slot 11: GetTimestampFrequency
    queue_clock_cal,   // slot 12: GetClockCalibration
};
```

### BUG-2: D3D12 CommandList vtbl incomplete for DXR

`g_cmdlist_vtbl[60]` — ID3D12GraphicsCommandList4 (what Cyberpunk needs for
raytracing) has 72+ methods. Our 60-slot vtbl is 12+ slots short for DXR-class
games.

**Current state of command list vtbl (60 slots):**
- Slots 0-8: correct (IUnknown, Object, DeviceChild, GetType)
- Slot 9-11: Close, Reset, ClearState — **implemented** ✅
- Slots 12-14: DrawInstanced, DrawIndexedInstanced, Dispatch — **implemented** ✅
- Slots 15-19: CopyBufferRegion, CopyTextureRegion, CopyResource, CopyTiles, ResolveSubresource — **cmd_noop** ❌
- Slots 20-25: IASetPrimitiveTopology, RSSetViewports, RSSetScissorRects, OMSetBlendFactor, OMSetStencilRef, SetPipelineState — **cmd_noop** ❌
- Slot 26: ResourceBarrier — **implemented** ✅
- Slots 27-46: ExecuteBundle, SetDescriptorHeaps, all root sig setters, IASetIndexBuffer, IASetVertexBuffers, SOSetTargets, OMSetRenderTargets — **cmd_noop** ❌
- Slots 47-48: ClearDSV, ClearRTV — **implemented** ✅
- Slots 49-55: ClearUAV*, Discard, queries, SetPredication — **cmd_noop** ❌
- Slots 56-58: SetMarker, BeginEvent, EndEvent — **cmd_noop** ❌
- Slot 59: ExecuteIndirect — **implemented** ✅

**Only ~10 of 60 slots are real. 50 are no-ops.** And 12+ methods are missing
entirely for DXR-class games (BeginRenderPass, EndRenderPass, DispatchRays,
BuildRaytracingAccelerationStructure, etc.).

### BUG-3: D3D12 Device vtbl incomplete

`g_device_vtbl[57]` — ID3D12Device has 90+ methods. We have 57 slots wired, of
which 21 are NOPs (view_noop), 10+ return S_OK without doing anything (`ok`),
5+ are E_NOTIMPL (`unimpl`).

**What's NOP in device vtbl:**
- Slots 17-22: CreateCBV/SRV/UAV/RTV/DSV/Sampler — **view_noop** (returns S_OK, creates nothing)
- Slots 23-24: CopyDescriptors — **view_noop**
- Slots 40, 41, 45, 46, 50: CopyResource, CopyBufferRegion, CopyTextureRegion, CopyTiles, ResolveSubresource — **ok (returns S_OK, does nothing)**
- Slots 30-33, 44, 48, 49, 52, 55: **unimpl (E_NOTIMPL)**

**What's real on device vtbl:**
- CreateCommandQueue, CreateCommandAllocator, CreateGraphicsPipelineState,
  CreateCommandList, CreateDescriptorHeap, CreateRootSignature,
  CreateCommittedResource (but resource is stub), CreateFence, CreateCommandList1

### BUG-6: D3D12 interface version QI chain stops at Device4

`dev_qi` answers `ID3D12Device` … `ID3D12Device4` (+ `ID3D12Object`) but NOT
`Device5`–`Device9`. DXR-class games (Cyberpunk, UE5) QI for `ID3D12Device4/5+`
for newer features (`CreateCommittedResource1`, meta-commands, shader-cache
APIs). A rejected QI here = the game takes its "no DX12 Ultimate support"
path or refuses to boot.

**Fix (trivial but must pair with BUG-3):** add the `Device5`–`Device9` GUIDs
to the QI chain AND implement the methods those versions add to the vtbl
(BUG-3's vtbl already stops at slot 57; Device4+ methods live at slots 58+).
Also verify the same version chains on `ID3D12GraphicsCommandList1–9` (games
QI these for `Present1`-class extensions) and `ID3D12Resource1/2`.

### BUG-4: D3D12 Resource stub objects

`CreateCommittedResource` exists but the created `ID3D12Resource` object is a
stub. Its `Map`/`Unmap`/`WriteToSubresource`/`ReadFromSubresource` do not
actually move data to the GPU. Games that upload textures via these methods
get garbage or nothing.

### BUG-5: No Xbox passthrough for D3D12

The D3D12 bridge has a `g_hooks.create_device` callback that, on the console,
should forward to the REAL Xbox `D3D12CreateDevice`. This is wired in
`swapchain.cpp` via `host_d3d12_create_device()`. BUT the rest of the D3D12
objects (device methods, queue methods, command list methods) DO NOT forward
to the real Xbox D3D12 — they return success without doing anything.

**Fix:** Every D3D12 method must, on console, call the corresponding method on
the real Xbox D3D12 object. On Linux gate, we need a software renderer fallback
(for testing only — the real target is console).

---

## THE COMPLETE GAP LIST — ALL remaining work

### PHASE 0: CONSOLE DEPLOYMENT (G1 — M0)

Everything is theory until deployed to Xbox TV.

**Tasks:**
1. **Pack and sign MSIX** — `tools/pack.sh` exists. Run it, sign with dev cert.
2. **Deploy to TV** — `tools/deploy.sh install`. Boot on TV. Screenshot. Pull `tiramisu.log`.
3. **Acceptance:** swapchain HRESULT 0, selftest `0x6494cb0d`, shelf visible on TV.
4. **JIT W^X probe** — first experiment. `VirtualAllocFromApp(PAGE_EXECUTE_READWRITE)`
   result: ALLOWED or DENIED. This decides the Mono JIT strategy (G3).
5. **SYSTEM route probe** — `LoadLibraryW` on d3dcompiler_47, d3d12, gdi32, shlwapi
   in the AppContainer. Which routes are real console APIs vs dead stubs?
6. **Audio probe** — can `IAudioClient`/MMDevice be reached in AppContainer?
7. **Input probe** — XInput/DInput work in AppContainer?
8. **Acceptance:** TV screenshot of Tiramisu UI + `tiramisu.log` showing successful boot.

**This is 1-2 days of work. It is the first and most important thing.**

---

### PHASE 1: D3D11 GAME COMPLETION

After G1 is proven, boot D3D11 games (Unity IL2CPP, Unity Mono, Godot, etc.)

#### 1A. GDI boot path (small, ~100 lines)
Games call GDI during window creation and splash screen rendering.
- **Missing:** `GetDC`, `ReleaseDC`, `BitBlt`, `StretchBlt`, `CreateCompatibleDC`,
  `CreateCompatibleBitmap`, `GetStockObject`
- **Impact:** Games that create DCs during boot crash or show blank splash screens.
- **Scope:** 100 lines in `bridge_gdi.cpp`, register in gdi32 module table.

#### 1B. Interlocked primitives in kernel32 table (~50 lines)
- **Missing:** `InterlockedIncrement`, `InterlockedDecrement`, `InterlockedExchangeAdd`
  (present in bridge_msvcrt.cpp but not in kernel32 kernel32 module table)
- **Impact:** Games using lock-free reference counting crash.
- **Scope:** 50 lines. Register in kernel32 table.

#### 1C. Memory copy primitives registration (~20 lines)
- **Missing:** `RtlCopyMemory`, `RtlMoveMemory`, `RtlZeroMemory` registered in
  kernel32/ntdll tables (present in d3dx9/d3d bridges but not exported via module table)
- **Impact:** Games calling these via `GetProcAddress(kernel32, "...")` get NULL.
- **Scope:** 20 lines.

#### 1D. System info in kernel32 table (~20 lines)
- **Missing:** `GetSystemInfo`, `GetNativeSystemInfo`, `GetEnvironmentVariableW`,
  `VirtualAllocEx`, `VirtualFreeEx` registered in kernel32 module table
  (implemented in bridge_sys.cpp / bridge_mem.cpp but not registered)
- **Impact:** Windows 10+ games that query system info fail detection.
- **Scope:** 20 lines.

#### 1E. Minor missing APIs (~300 lines total)
- `AllocConsole` / `FreeConsole` — for games that attach console
- `WakeAddress` — address-based sync (WaitOnAddress/WakeByAddress exist)
- `QueueUserWorkItem` — thread pool work items
- `SetProcessMitigationPolicy` — modern games (Cyberpunk, etc.)
- `SetThreadDescription` — thread naming for debuggers
- `SetThreadIdealProcessor` — CPU core pinning
- `SetProcessWorkingSetSize` — memory tuning
- **From the completeness audit (grep-verified missing):**
  - `nvapi64.dll` + `nvml.dll` (NVIDIA GPU DLLs — Cyberpunk/UE5 query them; honest stub surface)
  - `atiadl`/ADL (AMD display lib)
  - Job objects: `CreateJobObject`, `AssignProcessToJobObject`, `SetInformationJobObject`
  - `NtQuerySystemInformation`, `NtQueryInformationProcess` (ntdll)
  - `Module32First/Next` (toolhelp module enumeration)
  - `IDXGIFactory7`, `IDXGIOutput6` (extend d3d factory/output shims)
  - `CreateWaitCompletionPacket`
- **Scope:** ~300 lines total.

#### 1F. D3D11 completeness (G5)
- Swapchain resize/mode changes
- Multiple swapchains
- `IDXGIFactory6::EnumAdapterByGpuPreference`
- Full feature level contract
- Compute shaders (pass-through — verify)
- `d3dcompiler_47` runtime compile (SYSTEM route: prove it loads in AppContainer)
- Present/vsync/frame-pacing via host pump
- **Drive this with the G2 game's actual call trace.**

**Video HW decode (from round-2 audit):** `ID3D11VideoDevice`/`ID3D11VideoContext`,
`ID3D12VideoDevice` — cutscene decode for Bink-free games (300-500 lines, see Phase 9).

#### 1G. Mono JIT (G3 — Silksong-class)
The current wall for Unity Mono games:
- `mono-2.0-bdwgc.dll` JIT-compiles C# at startup
- Needs `PAGE_EXECUTE_READWRITE` memory or two-phase RW→RX
- **Use G1 probe result:**
  - If RWX ALLOWED on console → allow in `trt_VirtualAlloc`/`uwp_protect`
  - If RWX DENIED → implement UWP JIT pattern (alloc RW → guest writes → protect RX)
- **Add `FlushInstructionCache` + `VirtualQuery` bridges** (needed for JIT)
- Patch Mono's VirtualAlloc IAT if needed (we own the loader)
- **Corpus test:** Hollow Knight 1 (Mono, cheap), Silksong GOG (target)
- **Acceptance:** Mono game's `Assembly-CSharp.dll` code executes, ladder reaches
  window/D3D11, offline then console.

#### 1H. Audio — FMOD/WASAPI (G4)
- Unity 6 FMOD defaults to WASAPI
- **Probe on console:** Is `IAudioClient`/MMDevice reachable in AppContainer?
  - If yes → thin WASAPI bridge onto host sink (bridge already exists!)
  - If no → verify FMOD's DirectSound fallback initializes, lands PCM on existing
    console XAudio2 sink
- **Missing:** FMOD middleware bridge (1000-2000 lines) for Cyberpunk, Spider-Man, RDR2
- **Missing:** Audio decoders (Vorbis, AAC, HCA) for compressed game audio streams
- **Acceptance:** ≥5s of non-zero PCM in log from real Unity game.

#### 1I. Save game VFS
- VFS `C:\` writes must land in writable LocalState
- Verify RACCOIN's save lands and round-trips
- Route exe-dir writes to `C:\` overlay (Z: is read-only, Windows games write next
  to exe constantly)

#### 1J. PE resource directory (NEW — boot-relevant, 300-500 lines)
Games embed icons, cursors, splash bitmaps, manifest/config XML, and version
strings as Win32 resources and query them in WinMain — many fail before the
window without this.
- **Missing:** `.rsrc` directory parsing in `pe_loader.cpp` + `FindResourceW/Ex`,
  `LoadResource`, `LockResource`, `SizeofResource`, `EnumResource*`,
  `LoadStringW`, `LoadMenu`, `LoadBitmap` in the kernel32 bridge
- **Scope:** 300-500 lines (round-3 audit)

#### 1K. ntdll native surface (round-3 audit, ~250 lines)
- `NtCreateFile`/`NtReadFile`/`NtWriteFile`/`NtClose` — route onto VFS (150 lines)
- `NtCreateEvent`/`NtWaitForSingleObject`/`NtSignalAndWaitForSingleObject` (80 lines)
- `RaiseFailFastException` — UE fast-fail path (10 lines)
- `CoWaitForMultipleHandles` — STA pump (50 lines)

#### 1L. WinRT ThreadPool + D3DKMT (round-3 audit, ~200 lines)
- `Windows.System.Threading` (IThreadPoolStatics) — modern Xbox UWP scheduling (100 lines)
- `D3DKMTOpenAdapterFromLuid` + friends — engine adapter queries, honest stub (100 lines)
- Verify `Windows.Gaming.Input` event surface: GamepadAdded/Removed/FromGamepad (~30 lines)

---

### PHASE 2: D3D12 HEAVY HITTERS

This is the big gap. D3D12 games (Cyberpunk 2077, God of War, Spider-Man, RDR2,
Elden Ring, Black Myth: Wukong, GTA V-next-gen) require a COMPLETE D3D12 surface.

**On Xbox, this is easier than Linux:** the guest and host are the same AMD RDNA 2
architecture. Guest shaders target the same ISA. The bridge becomes a **passthrough**,
not a translation layer. Shaders execute natively.

#### 2A. D3D12 Device vtbl — Fill 39 missing/nop methods (~1500 lines)

**Current state:** 51/90 methods wired. The following are NOP/unimpl/ok:

| Slot | Name | Current | Needs |
|---|---|---|---|
| 17-22 | CreateCBV/SRV/UAV/RTV/DSV/Sampler | view_noop | **REAL** — descriptor creation |
| 23-24 | CopyDescriptors/CopyDescriptorsSimple | view_noop | **REAL** — descriptor heap population |
| 30-33 | CreateQuery, CreateReservedResource, CreateVideoDecoder, CreateVideoDecoderOutput | unimpl | Implement or leave as stub |
| 40 | CopyResource | ok (returns S_OK, does nothing) | **REAL** — resource-to-resource copy |
| 41 | CopyBufferRegion | ok | **REAL** — buffer region copy |
| 44 | CreateDeferredContext | unimpl | Implement |
| 45 | CopyTextureRegion | ok | **REAL** — texture region copy |
| 46 | CopyTiles | ok | **REAL** — tile copy (DX12.1) |
| 47 | CreatePipelineState | ok | **REAL** — PSO creation with shaders |
| 48 | CreateRootSignatureDeserializer | unimpl | Implement (for shader pipeline) |
| 49 | GetNodeCount | unimpl (but method 7 exists) | Check duplication |
| 50 | ResolveSubresource | ok | **REAL** — multisample resolve |
| 52 | CreatePipelineState (stream desc) | unimpl | **REAL** — extended pipeline state |
| 55 | CreateCommittedResource (variant) | unimpl | Check |

**Critical NOP→REAL mappings:**
- **Descriptors [17-24]:** Games create CBV/SRV/UAV/RTV/DSV/Sampler descriptors. NOP = no
  descriptors = shaders have no inputs = black screen. Must implement honest descriptor
  heap + descriptor range management.
- **CopyDescriptors [23-24]:** Descriptor table population. NOP = empty descriptor tables
  = draw calls reference invalid descriptors.
- **CopyResource [40]:** Resource-to-resource copy. ok = silent data loss.
- **CopyTextureRegion [45]:** Texture upload. ok = corrupted textures.
- **ResolveSubresource [50]:** Multisample resolve. ok = incorrect rendering.

#### 2B. ExecuteCommandLists on queue (~100 lines) — **THE MOST CRITICAL D3D12 METHOD**

The single method that sends commands to the GPU is NOT implemented. Without it,
commands are recorded but NEVER sent to the GPU. The guest creates queues, records
commands, but the GPU never executes them.

**On Xbox:** forward to the real Xbox `ID3D12CommandQueue::ExecuteCommandLists`.
On Linux gate: implement a stub that records commands in a command buffer and flushes
to a software renderer (for testing).

#### 2C. ID3D12CommandList — Command recording methods (~3000-5000 lines)

The command list's vtbl has NO recording methods implemented. Every draw call,
resource state transition, and shader binding goes through the command list:

**Graphics recording methods:**
- `GraphicsDispatch` — compute shaders
- `DrawInstanced`, `DrawIndexedInstanced` — the actual rendering
- `DrawInstancedIndirect`, `DrawIndexedInstancedIndirect`
- `DispatchGraph` — DX12.1 graphics workgraphs

**State setting methods (every draw call uses these):**
- `SetGraphicsRootSignature` — bind root signature
- `SetGraphicsRootDescriptorTable` — bind descriptor tables
- `SetGraphicsRootConstantBufferView` / `ShaderResourceView` / `UnorderedAccessView`
- `SetGraphicsRoot64BitConstantBufferView` / `ShaderResourceView` / `UnorderedAccessView`
- `SetGraphicsRootConstant` / `Constants64` — push constants
- `SetGraphicsRootDescriptorTable64`
- `IASetPrimitiveTopology` — triangle/line/point topology
- `IASetVertexBuffers` — vertex buffer binding
- `IASetIndexBuffer` — index buffer binding
- `IASetVertexAndIndexBuffers` — combined
- `OMSetRenderTargets` — render target + depth-stencil binding
- `OMSetBlendState` — alpha blending configuration
- `OMSetDepthStencilState` — depth testing configuration
- `RSSetViewports` — viewport rectangles
- `RSSetScissorRects` — scissor rectangles
- `RSSetDepthClipEnabled` — depth clip toggle
- `OMSetSampleMask` — MSAA sample mask
- `SOSetTargets` — stream output binding
- `DSSetShaders` — domain shader binding
- `HSSetShaders` — hull shader binding
- `GSSetShaders` — geometry shader binding
- `VSSetShaders` — vertex shader binding
- `PSSetShaders` — pixel shader binding
- `CSSetShaders` — compute shader binding
- `CSSetUnorderedAccessViews` — compute UAV binding
- `CSSetConstantBuffers` — compute CB binding
- `CSSetConstantBuffers64` — 64-bit CB binding
- `CSSetShaderResources` — compute SRV binding
- `CSSetSamplers` — compute sampler binding
- `CSSetBytes` — raw byte buffers

**Resource management:**
- `ResourceBarrier` — transition resource states (CRITICAL — games use this everywhere)
- `ClearRenderTargetView` — clear render target
- `ClearDepthStencilView` — clear depth/stencil
- `ClearUnorderedAccessViewFloat/Uint` — clear UAV
- `DiscardResource` — discard resource contents
- `BindComputeUnorderedAccessView` — compute UAV binding

**Query/timing:**
- `BeginQuery` / `EndQuery` — occlusion/timestamp queries
- `ResolveQueryData` — query results
- `EndRenderPass` / `BeginRenderPass` — render pass (DX12.1+)

**Compute methods:**
- `GraphicsDispatch` — GPU compute dispatch

#### 2D. Descriptor heap management (~500 lines)

Games allocate descriptor heaps and fill them with descriptors:
- CBV (Constant Buffer View) — shader uniforms
- SRV (Shader Resource View) — textures, buffers
- UAV (Unordered Access View) — write-back buffers
- RTV (Render Target View) — render targets
- DSV (Depth Stencil View) — depth buffers

**Missing implementations:**
- `CreateConstantBufferView` — needs real descriptor layout
- `CreateShaderResourceView` — needs real descriptor layout  
- `CreateUnorderedAccessView` — needs real descriptor layout
- `CreateRenderTargetView` — needs real descriptor layout
- `CreateDepthStencilView` — needs real descriptor layout
- `CreateSampler` — needs real descriptor layout
- `CopyDescriptors` / `CopyDescriptorsSimple` — batch descriptor population
- `GetDescriptorHandleIncrementSize` — already exists (method 15)

**On Xbox:** forward descriptor creation to real Xbox D3D12 descriptors. The
descriptor handle is a hardware pointer — the guest's handle IS the Xbox's handle.

#### 2E. D3D12 Resource creation (~500 lines)

`CreateCommittedResource` and `CreatePlacedResource` exist but the resources
themselves are stub objects. Need real resource objects that forward to the host.

**On Xbox:** forward `CreateCommittedResource`/`CreatePlacedResource` to real
`ID3D12Device::CreateCommittedResource`/`CreatePlacedResource`. The heap and
resource are real Xbox GPU resources.

#### 2F. Memory ceiling + MapViewOfFile eviction (AAA wall)

Heavy games use 5-8GB+ RAM. Current implementation has no ceiling.
- **Memory ceiling enforcement** — 5GB default for Game package
- **Commitment tracking** — `VirtualAlloc` with MEM_RESERVE only (no commitment)
- **VirtualFree with MEM_DECOMMIT** — keep VA range but free physical pages
- **VirtualQuery** — must report correct MEM_COMMIT/RESERVE/IMAGE regions
- **MapViewOfFile >64 maps** — eviction policy (LRU), ceiling enforcement
- **Memory tracking per map** — VA, size, offset, age for eviction
- **DirectX storage streaming** — >64 concurrent memory-mapped sections for asset streaming
- **If 5 GB is genuinely not enough:** evaluate the aerisarn/xbox-swap >5 GB
  allocator experiment (known UWP trick) — only when a real game OOMs, not before.

#### 2G. Archive parsers (per-game, 500-2000 lines each)

Heavy games ship encrypted/compressed archives, not loose files:

| Game | Archive Format | Size |
|---|---|---|
| Cyberpunk 2077 | REDengine 4 `.archive` | ~200GB |
| God of War | DECIMA `.bin` + `.res` | ~140GB |
| Spider-Man | Custompak | ~80GB |
| RDR2 | RAGE `.ydr`/`.ytd` + `.archive` | ~120GB |
| GTA V | RAGE `.ytd`/`.ydd` + `.xp3` | ~100GB |
| Witcher 3 | REDengine 3 `.blob` | ~50GB |
| Elden Ring | BKS archive | ~50GB |
| Black Myth: Wukong | UE5 `.pak` | ~130GB |

**Needed per game:** Archive format parser + virtual filesystem layer that serves
files from inside archives.

---

### PHASE 3: FMOD AUDIO BRIDGE (1000-2000 lines)

Cyberpunk, Spider-Man, RDR2, and many AAA games use FMOD Studio middleware.

**Missing FMOD exports:** `FMOD_System_Create`, `FMOD_System_Init`,
`FMOD_System_Update`, `FMOD_Channel_SetVolume`, `FMOD_Channel_GetPosition`,
`FMOD_Sound_SetPosition`, `FMOD_Channel_Play`, `FMOD_Channel_Stop`, etc.
(~200 functions across fmod.dll and fmodstudio.dll)

**Bridge strategy:** On Xbox, forward to real console audio APIs (WASAPI/XAudio2).
On Linux gate, output to WASAPI bridge sink.

**Audio decoder layer:** Games ship compressed audio (Vorbis, AAC, ADPCM, HCA).
WASAPI outputs PCM — need a decoder layer:
- Vorbis decoder (FMOD streams)
- AAC decoder (Bink video audio)
- HCA decoder (Sony games)

---

### PHASE 4: x86-32 SUPPORT (M7 / G8)

Native compat mode is DEAD on console (measured). Options:
1. **Boxedwine core** — proven x86-on-x64 UWP-class core (GPL-2 license check required)
2. **Custom x86→x64 JIT** — like Boxedwine but MIT-licensed (port semantics, no code)
3. **32→64 thunk** — translate 32-bit syscalls to 64-bit

Games with 32-bit components: PhysX 3.x middleware, FMOD 32-bit builds, legacy DLLs.

---

### PHASE 5: DIRECTSTORAGE (500 lines)

Games like Cyberpunk 2077 (DirectStorage capable) and future Xbox-ported titles
use DirectStorage for ultra-fast game loading:
- `D3D12QueueSetStablePowerState` — power management
- `D3D12CreateCommandQueue1` — enhanced command queue
- `DsCreateInProcessCommandList` — in-process command list
- `DsGetGraphicsEngineVersion` — engine version query
- `DsCreateResourceCommittedEx` — optimized resource creation

`dstorage.dll` DllMain already runs (RUN OK from corpus-run). Actual API calls
are not implemented.

---

### PHASE 6: UWP APPMODEL API (30 lines)

Xboxported games check UWP environment:
- `GetApplicationPackageFamilyName` — needs implementation in user32/kernel32
- `GetCurrentPackageFamilyName` — needs implementation
- `api-ms-win-appmodel-runtime` — bridge exists (apiset.cpp)

---

### PHASE 7: SHADER EXECUTION — THE DEEPEST GAP (on Linux; NOT on Xbox)

**IMPORTANT: On Xbox, shaders execute natively.** The guest and host are the same
AMD RDNA 2 architecture. Guest shader bytecode (HLSL/DXIL) runs on Xbox hardware.
The bridge forwards `CreateVertexShader`, `CreatePixelShader`, etc. to the real
Xbox D3D12 device — shaders compile and run on the real GPU.

**On Linux (offline gate only), shaders are a blocker:** the guest's compiled
shaders target a different GPU architecture. Solutions:
1. **Shader recoding:** Intercept `ID3DBlob*` from `CompileShader()`, translate HLSL→GLSL/HLSL, compile on host
2. **Shader caching:** Pre-compile game shaders on PC, ship compiled blobs, host intercepts and uses them
3. **Software rasterization:** Implement a software D3D12 renderer (slow but correct)

**On Xbox, this gap does not exist.**

---

### PHASE 8: NETWORKING (500-2000 lines)

Games that ship with online/multiplayer components need networking bridges.

**Missing:**
- `SteamNetworkingSockets` — Steam's modern networking (P2P, relay)
- `SteamMatchmaking` — lobby/matchmaking APIs
- `SteamFriends` — friend presence, invites
- `SteamRemoteStorage` — Steam Cloud save sync
- `SteamUGC` — Steam Workshop integration
- `EOS_Init` / `EOS_Platform_Init` — Epic Online Services (Fortnite, Rocket League, etc.)
- `EOS_P2P` / `EOS_lobby` — Epic P2P/lobby APIs
- `Xbox Live` (for Xbox-published games)
- `discord_game_sdk.dll` — Discord Rich Presence (in OrTMM corpus, currently fails handshake)
- Winsock expansion — bridge_ws2.cpp is present but may need more functions

**Not needed unless the game requires online play.** Single-player games boot fine without.

---

### PHASE 9: VIDEO PLAYBACK (500-1000 lines)

Games ship cutscenes and videos:
- **Bink video** — `binkw32.dll` in OrTMM corpus. Many AAA games (GTA V, RDR2, Cyberpunk).
- **Windows Media (WMV)** — via Media Foundation
- **H.264/HEVC** — via MFSourceReader (bridge_mf.cpp exists)
- **AVI/MP4 container parsing** — for older games
- **Ogg/VP8/VP9** — some indie games

**What's implemented:** Media Foundation COM bridge (`bridge_mf.cpp`) — IMFSourceReader, MFStartup, MFCreateAttributes, etc.

**What's missing:** Bink video codec, container parsers, actual decoder implementations.

**Impact:** Games that play intro cutscenes will skip or crash.

---

### PHASE 10: INPUT EXPANSION (200-500 lines)

**Missing:**
- `RegisterRawInputDevices` — games register for raw mouse/keyboard input
- `GetRawInputData` — read raw mouse packets
- `XInputSetState` — controller rumble/vibration
- `XInputGetStateEx` — extended state (trigger values, full button state)
- DualShock 4 / DualSense via Steam Input
- Keyboard layouts beyond QWERTY (AZERTY, QWERTZ, Dvorak, etc.)
- IME support for international input (Japanese, Chinese, Korean)
- Xbox GameBar input hooks
- **From round-2 audit:** `Windows.Xbox.Input` (legacy Xbox UWP input namespace — older Xbox UWP titles use it instead of Windows.Gaming.Input; ~100 lines), `GameBar`/`XboxGameBar` presence probe (~20 lines, honest false)

**Impact:** Games that use raw input for camera control (FPS games, mouse look) won't work without it.

---

### PHASE 11: CRASH HANDLING + DEBUGGING (100-300 lines)

**Missing:**
- `MiniDumpWriteDump` — crash reports
- `SetUnhandledExceptionFilter` — for custom crash handlers
- `WER_ReportRuntime` — Windows Error Reporting
- `CaptureStackBackTrace` — stack traces on crash
- SymFromAddr / SymInitialize — symbol resolution for crash dumps

**What's present:** `AddVectoredExceptionHandler`, `IsDebuggerPresent`, `OutputDebugStringW`, `wer.dll` bridge, `dbghelp.dll` bridge.

**Impact:** Games that install their own crash handlers will work. Games that expect WER to handle crashes will just exit.

---

### PHASE 12: PERFORMANCE (ongoing)

**Required for playable framerate:**
- Host-driven message pump at vsync
- `Present(1)` pacing
- FPS logging
- Guest-on-render-thread (separate thread for D3D11/D3D12 calls)
- Frame pacing / async compute
- Memory pooling for streaming assets
- Audio thread separation

**Impact:** Games that boot but run at <10 FPS are unplayable.

---

### PHASE 13: GPU-SPECIFIC FEATURES (1000-5000 lines)

For Cyberpunk, Black Myth: Wukong, and modern UE5 games:

- **DXR (DirectX Raytracing)** — `DispatchRays`, `BuildRaytracingAccelerationStructure`
- **Mesh shaders** — `DispatchMesh`
- **Sampler feedback** — `CreateSamplerFeedback`, `UpdateSamplerFeedback`
- **Variable rate shading** — `RSSetShadingRate`, `RSSetShadingRateImage`
- **DX12 Ultimate** — feature level 12.2
- **Mesh shaders (MS)** — `ID3D12GraphicsCommandList6::DispatchMesh`
- **Work graphs** — `ID3D12GraphicsCommandList10`
- **DirectStorage** — see Phase 5
- **HDR / scRGB** — `IDXGISwapChain3::SetColorSpace1`, HDR10 tone mapping
- **Auto HDR / VRR** — Xbox-specific display features

**Not needed for every game** — only DXR-class games need DXR, only UE5 needs mesh shaders, etc.

---

### PHASE 14: LOCALIZATION + UNICODE (100-500 lines)

- IME support (Japanese, Chinese, Korean input)
- Locale-aware string comparison (CompareStringEx, LCMapStringEx)
- Unicode normalization (NormalizeString)
- IDN (Internationalized Domain Names) — for networked games
- Right-to-left text (Arabic, Hebrew)
- Complex text layout (Arabic, Devanagari, Thai)

**What's present:** `MultiByteToWideChar`, `WideCharToMultiByte`, `CompareStringW/A/Ex/Ordinal`, `LCMapStringW/Ex`, `GetCPInfo`, `GetACP`, `GetOEMCP`, `GetSystemDefaultLCID`, `GetUserDefaultLCID`, `imm32.dll` bridge.

---

### PHASE 15: SECURITY + AUTHENTICATION (100-500 lines)

- **Authenticode / WinVerifyTrust** — for verifying game executables (bridge_wintrust exists in sys_extras)
- **bcrypt / ncrypt** — cryptographic APIs (bridges exist)
- **CertGetCertificateChain** — certificate validation
- **Credential Manager** — for storing passwords
- **CryptProtectData** — DPAPI encryption (save files, configs)
- **Secure APIs** — `SecureZeroMemory`, `RtlSecureZeroMemory`

**Not needed unless the game uses them.**

---

### PHASE 16: PROCESS + LAUNCHER INTEGRATION (200-500 lines)

Heavy games often launch through a launcher:
- **Rockstar Games Launcher** (GTA V, RDR2) — requires Rockstar services
- **Epic Games Launcher** (Fortnite, Rocket League)
- **EA App** (FIFA, Madden)
- **Ubisoft Connect** (Assassin's Creed, Far Cry)
- **GOG Galaxy** (Cyberpunk GOG version)
- **Steam** — already bridged (auth only)

**Missing:**
- `CreateProcessW` spawn hooks for launcher executables (present, works)
- Process handle inheritance for launcher↔game communication
- Named pipes for launcher↔game IPC (present in kernel32 bridge)
- Registry keys for launcher installation paths
- Environment variables set by launchers

**Workaround:** Run the game EXE directly, bypassing the launcher. Many games work this way.

---

### PHASE 17: DIRECTX DIAGNOSTICS (50-100 lines)

- `DxDiag` queries — `IDxDiagProvider`, `IDxDiagData`
- `DXGetErrorString` / `DXGetErrorDescription` — error messages
- `DXTrace` — debug output
- `IDXGIDebug` — D3D debug layer
- `ID3D12Debug` — D3D12 debug layer
- `ID3D12InfoQueue` — message queue for D3D12 debugging

**What's present:** `dbghelp.dll` bridge, `dxcore.dll` bridge in sys_extras.

**Impact:** Games that query D3D debug info will get NULL. Not critical for boot.

---

### PHASE 18: GAME ENGINE-SPECIFIC REQUIREMENTS (per-engine, 500-2000 lines)

**Unreal Engine 4/5:**
- `UE4Game.exe` / `GameAssembly.dll` (IL2CPP) or `Unreal.exe`
- APIs: `UWorld`, `UGameEngine`, `FRHICommandList`, `RHICreateShader`
- Graphics: D3D11, D3D12, or Vulkan (configurable)
- Audio: FMOD (UE4) or Wwise (UE5)
- **Known:** RACCOIN proves Unity IL2CPP; UE IL2CPP is similar

**Unity IL2CPP:**
- Proven (RACCOIN, Go Next)
- IL2CPP metadata loading, GC, mono runtime
- Boehm-Demers-Weiser GC with GetWriteWatch (bridged)

**Unity Mono:**
- Currently blocked (G3) — JIT memory
- Needs `mono-2.0-bdwgc.dll` with JIT compilation
- W^X probe result decides the path

**Unreal Engine 5 (Nanite, Lumen, VSM):**
- Requires DX12 Ultimate feature level
- Virtual Shadow Maps, Nanite geometry, Lumen global illumination
- Very complex shader surface

**DECIMA (God of War, Spider-Man):**
- D3D12 exclusive
- Custom engine, proprietary format
- Audio: custom (WASAPI underneath)

**RAGE (GTA V, RDR2):**
- D3D11 (GTA V) / D3D12 (RDR2 next-gen)
- Custom archives, SAGE audio
- Needs Steam/Rockstar integration

**Source 2 (CS2, Dota 2, Half-Life: Alyx):**
- D3D11 or Vulkan
- Custom archive format
- Steam integration required

**REDengine 4 (Cyberpunk 2077):**
- D3D12, DXR optional
- REDengine `.archive` format
- FMOD audio
- DirectStorage support

---

### PHASE 19: GAME-SPECIFIC INTEGRATION (per-game, 500-2000 lines each)

| Game | Priority | Blockers |
|---|---|---|
| Cyberpunk 2077 | HIGH | BUG-1, D3D12 command list, FMOD, REDengine `.archive`, DXR optional |
| God of War (2018) | HIGH | BUG-1, D3D12 command list, DECIMA archives |
| Spider-Man Remastered | HIGH | BUG-1, D3D12 command list, FMOD, Custompak |
| Elden Ring | HIGH | BUG-1, D3D12 command list, BKS archives |
| Red Dead Redemption 2 | HIGH | BUG-1, D3D12, SAGE audio, RAGE archives, Rockstar launcher |
| Black Myth: Wukong | MEDIUM | BUG-1, D3D12, Wwise, UE5 `.pak`, Nanite/Lumen (extreme) |
| GTA V | MEDIUM | D3D11 (easier), SAGE audio, RAGE archives |
| Witcher 3 | MEDIUM | D3D11, REDengine 3 `.blob`, FMOD |
| Resident Evil 4 | MEDIUM | D3D12, RE Engine archives |
| Monster Hunter: World | MEDIUM | D3D11, MT Framework |
| Skyrim Special Edition | LOW | D3D11, BSA archives, Creation Engine |
| Fallout 4 | LOW | D3D11, BA2 archives, Creation Engine |
| Hollow Knight | LOW | D3D11 (works after Phase 1) |
| Hades | LOW | D3D11 (works after Phase 1) |
| Stardew Valley | LOW | OpenGL or D3D11 |
| Terraria | LOW | XNA/FNA (D3D11) |

---

## COMPLETE PRIORITIZED WORK ORDER

### Sprint 0: Console M0 (1-2 days)
1. [ ] Pack + sign MSIX (`tools/pack.sh`)
2. [ ] Deploy to Xbox TV (`tools/deploy.sh install`)
3. [ ] Boot on TV, screenshot, pull log
4. [ ] W^X JIT probe — RWX allowed or denied?
5. [ ] SYSTEM route probe — which APIs are real on console?
6. [ ] Audio + input probe on console
7. [ ] **Acceptance: TV screenshot of Tiramisu UI running**

### Sprint 1: D3D11 game boot (1-2 weeks)
8. [ ] GDI: GetDC, ReleaseDC, BitBlt, StretchBlt, CreateCompatibleDC (~100 lines)
9. [ ] Interlocked: Increment, Decrement, ExchangeAdd in kernel32 table (~50 lines)
10. [ ] Memory copy primitives in kernel32 table (~20 lines)
11. [ ] System info calls in kernel32 table (~20 lines)
12. [ ] Minor: AllocConsole, WakeAddress, QueueUserWorkItem, mitigation, threading + audit items (nvapi/nvml/atiadl stubs, job objects, NtQuery*, Module32, IDXGI7/Output6) (~300 lines)
13. [ ] Mono JIT: FlushInstructionCache + VirtualQuery bridges, W^X probe fix
14. [ ] D3D11 completeness: swapchain resize, DXGI6, d3dcompiler_47, HW video decode (ID3D11VideoDevice)
15. [ ] **PE resource directory: .rsrc parsing + FindResource/LoadResource/LockResource/LoadString (1J, ~400 lines)**
16. [ ] ntdll native surface: NtCreateFile/NtReadFile/NtCreateEvent/NtWait + RaiseFailFastException + CoWaitForMultipleHandles (1K, ~250 lines)
17. [ ] WinRT ThreadPool + D3DKMT stubs + Gaming.Input event verify (1L, ~200 lines)
18. [ ] Audio: WASAPI probe on console, FMOD fallback verification
19. [ ] Save VFS: exe-dir writes route to C:\ overlay
20. [ ] **Acceptance: Hollow Knight or similar D3D11+Mono game boots on TV**

### Sprint 2: D3D12 heavy hitter bridge (4-8 weeks)
21. [ ] **D3D12 TRIANGLE GATE** — `tests/test_d3d12_triangle.cpp` + static-assert vtbl layouts; add to `corpus-run.sh` (the fixture that would have caught BUG-1)
22. [ ] **BUG-1 fix: D3D12 CommandQueue vtbl layout** (50 lines, unblocks all D3D12)
23. [ ] **BUG-6 fix: D3D12 version QI chain** — extend to Device5-9 + GraphicsCommandList1-9 + Resource1/2 (pairs with BUG-3)
24. [ ] D3D12 Device: NOP→real for descriptors (CreateCBV/SRV/UAV/RTV/DSV/Sampler)
25. [ ] D3D12 Device: NOP→real for CopyDescriptors
26. [ ] **D3D12 Queue: ExecuteCommandLists** (THE critical method, ~100 lines)
27. [ ] D3D12 Command List: DrawInstanced, SetRootSignature, ResourceBarrier, OMSetRenderTargets, etc. (~3000-5000 lines)
28. [ ] D3D12: CopyResource, CopyBufferRegion, CopyTextureRegion, ResolveSubresource (ok→real)
29. [ ] D3D12: CreatePipelineState with stream desc (unimpl→real)
30. [ ] D3D12: descriptor heap management (real descriptor creation)
31. [ ] D3D12: resource creation (CreateCommittedResource, CreatePlacedResource)
32. [ ] D3D12: Map/Unmap/WriteToSubresource/ReadFromSubresource on resources
33. [ ] D3D12: Xbox passthrough for every method (BUG-5)
34. [ ] Extend CommandList vtbl to ID3D12GraphicsCommandList4 (DXR) if needed
35. [ ] Memory ceiling + MapViewOfFile eviction (>64 maps, LRU)
36. [ ] **Acceptance: TRIANGLE GATE 12/12 markers offline, then Cyberpunk 2077 or God of War boots to rendered frame on TV**

### Sprint 3: Full game boot (4-8 weeks)
37. [ ] FMOD audio bridge (1000-2000 lines)
38. [ ] Wwise audio bridge (for RACCOIN and other games)
39. [ ] Audio decoders: Vorbis, AAC, HCA, ADPCM
40. [ ] Game archive parsers (REDengine, DECIMA, RAGE, UE5 .pak, BKS, RE Engine)
41. [ ] DirectStorage API
42. [ ] UWP AppModel APIs
43. [ ] Networking: Steam, Epic, Xbox Live (as needed)
44. [ ] Video: Bink, WMV, H.264/HEVC, container parsers
45. [ ] Input expansion: raw input, rumble, DS4/DS5, IME
46. [ ] Crash handling: MiniDump, SetUnhandledExceptionFilter, WER
47. [ ] Performance: pump pacing, frame timing, memory pooling
48. [ ] Per-game integration: Cyberpunk, God of War, Spider-Man, Elden Ring, RDR2
49. [ ] **Acceptance: Full Cyberpunk 2077 gameplay on TV**

### Sprint 4: Legacy + 32-bit (ongoing)
50. [ ] x86-32 support (Boxedwine port or custom JIT)
51. [ ] D3D9/D3D8 game expansion
52. [ ] OpenGL bridge validation against Godot games
53. [ ] Game-specific tuning (per-engine bugs)

### Sprint 5: GPU-specific features (as needed)
54. [ ] DXR (ray tracing) for Cyberpunk RT, Black Myth Wukong
55. [ ] Mesh shaders for UE5 (Nanite)
56. [ ] Sampler feedback, VRS, DX12 Ultimate
57. [ ] HDR / Auto HDR / VRR
58. [ ] Work graphs (DX12.2)

---

## WHAT EVERY HEAVY GAME NEEDS (per-game analysis)

### Cyberpunk 2077 (REDengine 4)
- D3D12 (DXR for RT mode, D3D12 without RT)
- ~200 GB of `.archive` files (REDengine format, compressed + encrypted)
- FMOD Studio audio
- DirectStorage
- x64 only, no 32-bit components
- Needs DXR (Build/DispatchRays) for ray tracing; standard mode is D3D12 only
- **Blockers:** BUG-1 (queue vtbl), D3D12 command list, FMOD, REDengine `.archive`, DXR optional

### God of War (2018/2021 PC port)
- DECIMA engine (Sony proprietary)
- D3D12 only
- ~140 GB of `.bin`/`.res` files (DECIMA format)
- Custom audio (WASAPI underneath)
- XInput for controller
- DX12 Ultimate features (DXR, mesh shaders, sampler feedback)
- **Blockers:** BUG-1, D3D12 command list, DECIMA `.bin`/`.res` parser

### Spider-Man Remastered (Nixxes port)
- DECIMA engine (Sony/Nixxes)
- D3D12 only
- ~80 GB of Custompak archives
- FMOD audio
- XInput, keyboard+mouse
- **Blockers:** BUG-1, D3D12 command list, FMOD, Custompak parser

### Elden Ring
- FromSoftware custom engine
- D3D12
- ~50 GB BKS archives
- Custom audio (WASAPI/XAudio2 underneath)
- XInput, keyboard+mouse
- **Blockers:** BUG-1, D3D12 command list, BKS parser

### Red Dead Redemption 2
- RAGE engine (Rockstar)
- D3D12 (next-gen) or D3D11 (legacy)
- ~120 GB of RAGE archives
- SAGE audio (Rockstar custom)
- XInput, keyboard+mouse
- Needs Rockstar Games Launcher / Steam integration
- **Blockers:** BUG-1, D3D12, SAGE audio, RAGE parser, launcher integration

### Black Myth: Wukong
- Unreal Engine 5
- D3D12, DXR, Nanite, Lumen, VSM
- ~130 GB UE5 `.pak` archives
- Wwise audio
- XInput
- **Blockers:** BUG-1, D3D12 command list, Wwise, UE5 `.pak` parser, Nanite/Lumen (EXTREME)

### Resident Evil 4 / Village / Monster Hunter
- RE Engine (Capcom) / MT Framework (Monster Hunter)
- D3D12 (RE Engine), D3D11 (MT Framework)
- RE Engine `.pak` archives
- Custom audio
- **Blockers:** BUG-1, D3D12 command list, RE Engine `.pak` parser

### GTA V
- RAGE engine
- D3D11 (legacy) or D3D12 (next-gen)
- RAGE archives
- SAGE audio
- Needs Online components (networking)
- **Blockers:** SAGE audio, RAGE parser, Online networking (can work in single-player)

### Skyrim Special Edition / Fallout 4
- Creation Engine (Bethesda)
- D3D11
- BSA/BA2 archives
- FMOD audio
- **Blockers:** BSA/BA2 parser, Creation Engine specifics

### Hollow Knight / Silksong
- Unity Mono (not IL2CPP)
- D3D11
- Needs JIT memory (G3)
- **Blockers:** Mono JIT, W^X probe result

### Stardew Valley / Hades / indie D3D11
- Various engines (XNA/FNA, custom)
- D3D11 or OpenGL
- **Blockers:** Mostly ready; OpenGL needs validation for some

---

## HOW A HEAVY GAME ACTUALLY BOOTS (step-by-step, Cyberpunk 2077)

This is the exact boot path, step-by-step, with what works and what doesn't:

```
STEP 1:  CreateProcessW(guest)
         ✅ WORKS — spawn hook models a PyInstaller-style child process

STEP 2:  PE loader (map, relocate, bind, TLS, W^X seal)
         ✅ WORKS — 921-line PE64 loader, proven with RACCOIN + Go Next

STEP 3:  TLS callbacks (thread-local storage init)
         ✅ WORKS — per-thread TEB + dynamic .tls template

STEP 4:  CRT startup (msvcrt, ucrtbase, vcruntime140)
         ✅ WORKS — msvcrt bridge is 2782 lines

STEP 5:  WinMain entry
         ✅ WORKS — proven with RACCOIN + Go Next

STEP 6:  CoInitializeEx (COM boot)
         ✅ WORKS — bridge_ole.cpp has CoInitialize, CoInitializeEx,
         CoCreateInstance, CoTaskMem, DCOM class factory

STEP 7:  LoadLibrary of engine DLLs (GameAssembly.dll, UnityPlayer.dll, etc.)
         ✅ WORKS — VFS game-dir search finds sibling DLLs

STEP 8:  D3D12CreateDevice or D3D11CreateDevice
         ✅ WORKS — returns real Xbox D3D12/D3D11 device

STEP 9:  Resource creation (CreateCommittedResource, textures, buffers)
         ⚠️ PARTIAL — device methods exist, but resources are stubs
         ❌ Map/Unmap don't actually move data

STEP 10: Shader compilation (D3DCompile or D3D12 Serialize PSO)
         ❌ BLOCKER on Linux — guest shaders target guest GPU; they don't work there
         ✅ On Xbox — shaders execute natively (AMD RDNA 2)

STEP 11: Command list recording (DrawInstanced, ResourceBarrier, etc.)
         ❌ BLOCKER — 50 of 60 cmdlist slots are no-ops
         Only ~10 methods are real (Close, Reset, ClearState, DrawInstanced,
         DrawIndexedInstanced, Dispatch, ResourceBarrier, ClearDSV, ClearRTV,
         ExecuteIndirect)

STEP 12: ExecuteCommandLists (send commands to GPU)
         ❌ CRITICAL BLOCKER — BUG-1: queue vtbl is WRONG.
         Game's ExecuteCommandLists call goes to queue_update_tile (no-op).
         Game never renders anything.

STEP 13: Swapchain Present
         ✅ WORKS (D3D11 path) — swapchain_present() calls Present(1,0)
         ⚠️ PARTIAL (D3D12) — acquire_d3d12_swapchain exists, needs BUG-1 fix

STEP 14: Audio init (FMOD/Wwise → WASAPI/XAudio2)
         ❌ BLOCKER for AAA — FMOD and Wwise not bridged
         ✅ WORKS for XAudio2 (bridge_audio.cpp)
         ✅ WORKS for WASAPI (bridge_wasapi.cpp)

STEP 15: Input init (XInput/DInput/raw input)
         ⚠️ PARTIAL — XInput bridge exists, raw input needs work

STEP 16: Archive loading (.archive files, textures, models, audio)
         ❌ BLOCKER — no REDengine/DECIMA/RAGE/UE5 archive parser
         Games can't load their own game data

STEP 17: Game loop message pump
         ✅ WORKS — GetMessage, PeekMessage, DispatchMessage all present

STEP 18: First rendered frame on TV
         ❌ BLOCKED at STEP 12 (ExecuteCommandLists no-op)
```

**The boot stops at STEP 12** because the queue vtbl is wrong. Once BUG-1 is
fixed, we'd stop at STEP 11 (command recording no-ops). Once those are filled
in, we'd stop at STEP 16 (archive parsers).

**The order of fixes to get Cyberpunk booting:**
1. BUG-1 — Fix queue vtbl (50 lines, unblocks STEP 12)
2. Command list recording — Fill 50 no-op slots (3000-5000 lines, unblocks STEP 11)
3. Resource Map/Unmap — real CPU→GPU data transfer (500 lines, unblocks STEP 9)
4. FMOD + Wwise — audio middleware bridge (1000-2000 lines, unblocks STEP 14)
5. Archive parser — REDengine `.archive` (500-2000 lines, unblocks STEP 16)
6. **Cyberpunk boots to first frame**

---

## ESTIMATED TOTAL EFFORT

| Phase | Lines | Time |
|---|---|---|
| BUG-1 fix (CommandQueue vtbl) | 50 | 1 hour |
| Console M0 (deploy + probes) | — | 1-2 days |
| D3D11 completion (1A-1I) | 2000 | 1-2 weeks |
| D3D12 heavy hitters (2A-2I) | 8000-12000 | 4-8 weeks |
| FMOD + Wwise + decoders | 2000-4000 | 2-4 weeks |
| Archive parsers (8+ games) | 4000-16000 | 4-8 weeks |
| x86-32 | 2000-4000 | 4-8 weeks |
| DirectStorage | 500 | 1 week |
| Networking | 500-2000 | 1-2 weeks |
| Video | 500-1000 | 1 week |
| Input expansion | 200-500 | 1 week |
| Crash handling | 100-300 | 1 week |
| Performance | 1000-3000 | ongoing |
| GPU-specific (DXR, mesh shaders) | 1000-5000 | 2-6 weeks |
| Localization | 100-500 | 1 week |
| Security | 100-500 | 1 week |
| Launcher integration | 200-500 | 1 week |
| **TOTAL** | **25000-55000** | **3-12 months** |

With Bug-1 fixed + console M0 + Phase 1: **you can boot a D3D11 game on Xbox in ~2 weeks.**
With Phase 2: **you can boot Cyberpunk 2077 on Xbox in ~2 months.**
With Phase 3+: **you can boot any AAA game on Xbox in ~6-12 months.**

---

## DEFINITION OF DONE

**A commercial game we own** (start with a GOG/itch DRM-free x64 D3D11 indie,
then Hollow Knight 1 (Mono), then a D3D12 title like Cyberpunk 2077) **launches
from the shelf, renders on the TV, takes controller input, plays audio, writes
its save, and quits cleanly** — verified by a TV screenshot and `tiramisu.log`
showing zero unexplained faults. That is the bar. Everything else in this document
is the path to it.

---

## Key files to know

| File | Purpose |
|---|---|
| `src/trt/pe_loader.cpp` | PE64 loader: map, relocate, import-bind, TLS, W^X seal |
| `src/trt/bridge_kernel32.cpp` | kernel32.dll surface (490+ fns registered) |
| `src/trt/bridge_kernel32_gap.cpp` | kernel32 additional fns (156+ fns) |
| `src/trt/bridge_user32.cpp` | user32.dll surface (~700 fns) |
| `src/trt/bridge_user32_gap.cpp` | user32 additional fns (~400 fns) |
| `src/trt/bridge_thread.cpp` | Thread pool, sync, IOCP, fibers (97 fns) |
| `src/trt/bridge_d3d.cpp` | D3D11/DXGI pass-through bridge |
| `src/trt/bridge_d3d12.cpp` | D3D12 stub bridge (51/90 vtbl methods) |
| `src/trt/bridge_mem.cpp` | VirtualAlloc/VirtualFree/CreateFileMapping/MapViewOfFile |
| `src/trt/bridge_ntdll.cpp` | NtAllocateVirtualMemory, NtFreeVirtualMemory |
| `src/trt/bridge_audio.cpp` | XAudio2 COM bridge |
| `src/trt/bridge_wasapi.cpp` | WASAPI COM audio stack |
| `src/trt/bridge_ole.cpp` | COM/OLE/CoInitialize/CoCreateInstance |
| `src/trt/bridge_gdi.cpp` | GDI 48+ functions (missing GetDC/BitBlt) |
| `src/trt/bridge_vulkan.cpp` | Vulkan→D3D11 translation |
| `src/trt/bridge_opengl32.cpp` | OpenGL→D3D11 translation (156KB, 3997 lines) |
| `src/trt/bridge_steam.cpp` | Steam auth-only bridge |
| `src/trt/bridge_winrt.cpp` | WinRT, DispatcherQueue, dwrite |
| `src/trt/bridge_mf.cpp` | Media Foundation COM |
| `src/trt/bridge_sys.cpp` | System info, file operations |
| `src/host/swapchain.cpp` | Host D3D11/D3D12 swapchain on CoreWindow |
| `src/host/audio.cpp` | Host audio output |
| `src/host/main.cpp` | Host application + W^X probe |
| `d3d9_backend.hpp` | D3D9 hook interceptor (70KB) |
| `vk_backend.hpp` | Vulkan backend (38KB) |
| `gl_backend.hpp` | OpenGL backend (38KB) |
| `tools/build-test.sh` | Offline gate: compile + test |
| `tools/corpus-smoke.sh` | DLL load matrix test |
| `tools/corpus-run.sh` | DLL DllMain + EXE boot depth ladder |
| `tools/build-host.sh` | Build host tiramisu.exe |
| `tools/pack.sh` | MSIX packaging + signing |
| `tools/deploy.sh` | Deploy to Xbox TV |
| `tools/audit_boot.py` | Import audit (fix __main__ guard) |
| `GAP-MAP.md` | Living status document |

## Working protocol (every task)

1. **Measure first**: reproduce the failure with a corpus PE + the depth ladder /
   audit (`tools/audit_imports.py`, `tests/audit_boot.cpp`). Name the exact
   `dll!symbol` / HRESULT / last-error where the guest dies.
2. **Close the named gap** in the bridge, with SDK-exact vtables/sizes (static_assert
   offsets like the rest of the codebase).
3. **Add the test** that would have caught it; keep every prior test green
   (`tools/build-test.sh` + `corpus-run.sh` before touching anything console).
4. **Console-verify** with `tools/deploy.sh` + screenshot + `tiramisu.log`.
5. **Write the evidence line** in `docs/03-evidence.md` and update the README
   status table. Commit.

If an approach fails twice in a row, stop, read the raw error, and pivot — do not loop.
The fallback hierarchy is: bridge it BUILTIN → honest fail-visible STUB → named
console probe. Never fake success.

### PHASE 20: GAME ACQUISITION LOGISTICS (process, not code)

How game files actually get onto the Xbox — the old contract's G6, kept:

- **No depot downloader exists.** `tools/steam_depot.py` exists (auth-only QR
  login built). Sideload game files via the web transfer UI (streams to disk;
  fine for GBs) from games we own.
- **Client-less Steam builds:** drop `steam_appid.txt` in the game dir during
  transfer. No Valve/MS binaries in git, ever.
- **steam_api64.dll precedence:** `trt_resolve_full` currently prefers the
  game's own steam_api64.dll over our BUILTIN auth bridge — decide explicitly
  per game which wins and log the decision.
- **`corpus-local/` is gitignored** — real games never commit.
- **Xbox storage budget:** Dev Mode games get 5 GB by default; the "Treat UWP
  apps as games" toggle gives the game class. Large installs (Cyberpunk 200 GB)
  do NOT fit — plan per-game what subset of archives is required for boot vs
  gameplay, and stream the rest from... nowhere. Be honest: a 200 GB game needs
  an external-USB or trimmed-install strategy measured in Sprint 3, not assumed.

---

## COMPLETENESS AUDIT (2026-11-15, verified by grep across all 58 bridge files)

### Verified-MISSING APIs (grep-confirmed absent from every bridge):

| Item | Impact | Scope |
|---|---|---|
| `nvapi64.dll` (NVIDIA GPU DLL) | Games call it for driver features, Reflex, SLI. Cyberpunk/UE5 query it. | 100-300 lines (honest stub surface) |
| **BUG-6: D3D12 version QI chain** (Device5-9 unhandled) | DXR games QI Device4/5+ → E_NOINTERFACE → "no DX12U support" path | 20 lines GUIDs + BUG-3 vtbl methods |
| `ID3D11VideoDevice`/`ID3D11VideoContext`, `ID3D12VideoDevice` (HW video decode) | Cutscene/cinematic decode (Bink-free games use MF+HW decode) | 300-500 lines |
| `GameBar` / `XboxGameBar` presence probe | Xbox games detect Game Bar to toggle overlay features | 20 lines (honest false) |
| `Windows.Xbox.Input` (legacy Xbox UWP input ns) | Older Xbox UWP titles use it instead of Windows.Gaming.Input | 100 lines |
| `SetFileCompletionNotificationModes` | Async I/O tuning (skips completion packets) | 10 lines |
| `ITaskbarList3` | Launcher progress UI (cosmetic on Xbox) | 30 lines |
| `IFileOpenDialog` / `IShellItem` | Mod tools / save import pickers (rare at boot) | 100 lines |
| `nvml.dll` (NVIDIA monitoring) | Perf overlays, GPU stats | 50 lines |
| `atiadl`/ADL (AMD display lib) | AMD perf queries | 50 lines |
| Job objects (`CreateJobObject`, `AssignProcessToJobObject`, `SetInformationJobObject`) | Games cap memory/threads via jobs | 100-200 lines |
| `NtQuerySystemInformation` | System info queries, handle tables | 100-200 lines |
| `NtQueryInformationProcess` | Process info (games detect environment) | 50 lines |
| `Module32First/Next` (toolhelp module enum) | Games enumerate loaded DLLs (anti-tamper checks, plugin systems) | 50 lines |
| `IDXGIFactory7` | Newer DXGI (factory6 exists) | 20 lines |
| `IDXGIOutput6` | HDR/display per-output info | 30 lines |
| `CreateWaitCompletionPacket` | Async completion (rare) | 30 lines |

### Verified-PRESENT (grep-confirmed, no work needed):
- `MMDeviceEnumerator` (audio device enum) ✅
- `SetFullscreenState` (RACCOIN's shutdown path — proven S_OK) ✅
- `SetColorSpace1` (HDR color space) ✅
- `GetProcessAffinityMask` ✅ · `timeBeginPeriod` ✅ · `GetDiskFreeSpaceEx` ✅
- `Process32First` ✅ · `HeapWalk/Validate` ✅ · `GetLogicalProcessorInformation` ✅
- **D3D12 Agility SDK** (`D3D12Core.dll` game-shipped loading) ✅ — bridge_d3d12.cpp
- `DuplicateHandle` ✅ · `XInputGetBatteryInformation` ✅ · `XInputGetAudioDeviceIds` ✅

### Round-2 audit (subsystems, 2026-11-15) — verified PRESENT, do NOT re-implement:
- **`Windows.Gaming.Input` (WinRT Gamepad)** ✅ — bridge_winrt.cpp + bridge_hid.h (this is the Xbox UWP input stack, not just XInput)
- `Windows.Gaming.Input.Custom` (RacingWheel) ✅ — bridge_gameinput.cpp
- `RoInitialize` / `RoGetActivationFactory` (WinRT activation) ✅ — bridge_winrt.cpp
- WinRT async (`IAsyncOperation` in host main.cpp) ✅
- `Present1` (dirty rects) + flip-model (`FLIP_SEQUENTIAL`/`FLIP_DISCARD`) ✅ — bridge_d3d.cpp
- `IDXGISwapChain2/3/4` QI ✅ · `ID3D11Device1` ✅ (host passthrough) · `ID3D12Device1` ✅ (QI chain, see BUG-6 for 5-9)
- `SetFileInformationByHandle` / `GetFileInformationByHandleEx` / `MoveFileEx` / `ReplaceFile` / `GetLongPathName` / `GetShortPathName` ✅ — bridge_kernel32_gap.cpp
- **ETW** (`EtwEventRegister`/`EtwEventWrite`) ✅ — bridge_ntdll.cpp
- **D3D11On12** (`D3D11On12CreateDevice`) ✅ — bridge_d3d.cpp + swapchain.cpp
- `GetLogicalDrives` / `GetDriveType` ✅ · `PlaySound` ✅ — bridge_winmm.cpp

### Round-3 audit (deep sweep, 2026-11-15) — FINAL additions:

**NEW subsystem gap (boot-relevant):**
| Item | Impact | Scope |
|---|---|---|
| **PE resource directory** — `FindResourceW/Ex`, `LoadResource`, `LockResource`, `SizeofResource`, `EnumResource*`, `LoadStringW`, `LoadMenu`, `LoadImageW` on exe resources | **Boot-relevant**: games embed icons, cursors, splash bitmaps, manifest/config XML, version strings as Win32 resources and query them in WinMain. No resource directory parsing = many games fail before the window. | 300-500 lines (parse `.rsrc` directory in pe_loader + resource APIs in kernel32 bridge) |

**ntdll native surface (games/anti-probe layers call these directly):**
| Item | Impact | Scope |
|---|---|---|
| `NtCreateFile`/`NtReadFile`/`NtWriteFile`/`NtClose` | Native IO — some engines bypass CreateFileW | 150 lines (route onto VFS) |
| `NtCreateEvent`/`NtWaitForSingleObject`/`NtSignalAndWaitForSingleObject` | Native sync | 80 lines |
| `RaiseFailFastException` | UE/engines fast-fail crash path | 10 lines |

**Misc:**
| Item | Impact | Scope |
|---|---|---|
| `CoWaitForMultipleHandles` | STA message-pump waiting (Unity/UE use) | 50 lines |
| `Windows.System.Threading` (WinRT ThreadPool) | Modern Xbox UWP games schedule work via it | 100 lines |
| `D3DKMT*` queries (kernel graphics escapes) | Engines query adapter/LUID via D3DKMTOpenAdapterFromLuid | 50-100 lines honest stub |
| D3D11/DXGI `InfoQueue` (debug) | debug-only, pairs with Phase 17 | 50 lines |
| Verify `Windows.Gaming.Input` event surface (`GamepadAdded`/`GamepadRemoved`/`FromGamepad`) | namespace present; event wiring unverified | verify, ~30 lines |

Round-1/2 false alarms confirmed fine: SAS/secure-attention (host-owned), MultiplaneOverlay/DirectDisplay (display tech, games don't call), RtlRaiseStatus (label artifact — RtlUnwindEx/RtlVirtualUnwind ARE present per GAP-MAP; only RtlRaiseStatus itself is absent, folded into EH polish).

### The D3D12 TRIANGLE GATE (the missing acceptance test — do FIRST in Phase 2)

**Problem:** Every D3D12 fix (BUG-1, command list, descriptors) currently has NO
offline acceptance test. You'd be fixing vtables blind and deploying to console
to find out.

**Solution:** `tests/test_d3d12_triangle.cpp` — a synthetic freestanding guest
(clang x64, like `guest_game.c`) that walks the FULL D3D12 render path:

```
 1. D3D12CreateDevice            → S_OK, device != null
 2. device->CreateCommandQueue   → queue != null
 3. device->CreateCommandAllocator → alloc != null
 4. device->CreateCommandList(alloc) → list != null
 5. device->CreateRootSignature  → rootsig != null
 6. device->CreateGraphicsPipelineState (tiny DXIL VS+PS blobs) → pso != null
 7. device->CreateCommittedResource (DEFAULT, vertex buffer) → res != null
 8. list->SetGraphicsRootSignature / SetPipelineState / IASetVertexBuffers
 9. list->DrawInstanced(3,1,0,0)
10. list->Close
11. queue->ExecuteCommandLists(1, &list)   ← validates BUG-1 fix (slot 8!)
12. device->CreateFence + queue->Signal + fence->SetEventOnCompletion → completes
13. CreateSwapChainForCoreWindow via the factory shim → host swapchain
14. swapchain->Present(1,0)
```

**Ladder markers** (same pattern as the EXE ladder): `D3D12 DEVICE OK / QUEUE OK /
ALLOC OK / LIST OK / ROOTSIG OK / PSO OK / RESOURCE OK / DRAW RECORDED / LIST
CLOSED / ECL EXECUTED / FENCE SIGNALED / PRESENTED`.

**Gate:** add to `tools/corpus-run.sh` alongside the EXE stage. Every D3D12
bridge change must keep this green offline before console work. This is to
D3D12 what `test_game_boot.cpp` is to the PE loader — the fixture that would
have caught BUG-1.

**Static-assert the vtbl layouts too** (the codebase already does this for
structs): slot-by-ABI checks that `g_queue_vtbl[8]` is `ExecuteCommandLists`,
so a future reorder can't silently regress it.

### Anti-cheat honesty note (which games are hard-blocked, by rule)

Contract-prohibited: **no DRM/anti-cheat bypass.** Practical consequence:
- Elden Ring / GTA V Online / RDR2 Online / CoD etc. — online modes with
  EAC/BattlEye are **out of scope** (the anti-cheat refuses to init; that must
  be a *named* failure, never bypassed). Single-player may still work where the
  anti-cheat is optional or absent.
- Denuvo-protected builds refuse to start; the log must name the check that
  refused. Use DRM-free editions (GOG/itch) for those titles.
- This is a feature of the contract, not a bug — the per-game table above
  already reflects it; don't burn a session rediscovering it.

---

## Donor projects — pull semantics, not code (license-check before vendoring)

- **Wine** (LGPL): user32/kernel32/shell32 semantics, spec files as checklist
- **mono / Unity-Technologies mono** (MIT-ish): JIT memory manager, W^X options
- **Boxedwine** (GPL-2): x86-on-x64 UWP core — G8, licensing decision first
- **D3D9On12, cnc-ddraw** (MIT): D3D9/D3D8 bridge reference
- **FAudio** (MS-PL): XAudio2 reference behavior
- **xboxnative donor lane**: console evidence for AppContainer SYSTEM routes
- **uwp-crossbuild** (already wired): toolchain

### Additional donors for heavy hitters:

- **DXVK** (Zink, MIT): D3D9/10/11→Vulkan — reference for D3D11 semantics, excellent vtable documentation
- **VKD3D-Proton** (LGPL): D3D12→Vulkan — reference for D3D12 semantics, has DXR knowledge
- **Proton / Wine-Staging** (LGPL): Windows API semantics on Linux
- **Mesa / Zink** (MIT): OpenGL-on-D3D12 — useful for OpenGL bridge validation
- **xenia** (BSD): Xbox 360 emulator — has D3D12 knowledge, useful for Xbox-specific behavior
- **XBSX2 / PCSX2** (GPL): PS2 emulator with D3D12 backend
- **dosbox-pure-uwp**: UWP W^X knowledge, AppContainer patterns
- **PPSSPP**: UWP W^X knowledge, JIT pattern, one of the few working UWP JIT implementations
- **RPCS3** (GPL): PS3 emulator — has D3D12 + Vulkan backends
- **yuzu / Ryujinx** (GPL/MIT): Switch emulators — D3D11/D3D12/Vulkan backends
- **Scalibq / D3D12 translation layers** (various): D3D12 command list reference implementations
- **MoltenVK** (Apache-2.0): Vulkan-on-Metal — reference for translation layer architecture
- **FLAT IS JUSTICE / open-source UWP games**: UWP-specific game patterns

### Reference documentation:

- **MSDN D3D12 documentation** — authoritative source for all D3D12 methods
- **MSDN DXGI documentation** — authoritative source for DXGI methods
- **Microsoft DirectX Graphics Samples** — GitHub repo with working D3D12 code
- **UE4/UE5 source** (Epic GitHub) — reference for engine patterns
- **Unity IL2CPP internals doc** (Unity-Technologies) — IL2CPP runtime behavior
- **ILSpy / dnSpy** (MIT) — for inspecting managed assemblies
- **x64 Windows Internals** — for TEB/PEB/SEH/KPCRB details
- **"Practical Reverse Engineering"** (Wiley) — x86/x64/ARM reverse engineering
- **"Windows Internals Part 1 & 2"** (Microsoft Press) — Windows internals reference
