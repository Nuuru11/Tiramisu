#!/usr/bin/env python3
"""tiramisu auto-compat: scan a PE64 game and report API coverage.

Reads a game executable, enumerates its imports (regular + delay), and
cross-references them against the function names implemented in tiramisu's
bridge layer (src/trt/bridge_*.cpp). Prints a per-DLL coverage report and
writes a machine-readable JSON summary.

Usage:
    python3 tools/auto-compat.py <game.exe> [--src src/trt] [--out report.json]
"""
import argparse, json, os, re, struct, sys

def rva2off(data, rva):
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    optsz = struct.unpack_from('<H', data, pe + 20)[0]
    sec0 = pe + 0x18 + optsz
    for i in range(nsec):
        s = sec0 + i * 40
        vsz, va = struct.unpack_from('<II', data, s + 8)
        rawsz, raw = struct.unpack_from('<II', data, s + 16)
        if va <= rva < va + max(vsz, rawsz):
            return rva - va + raw
    return None

def imports(path):
    data = open(path, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    opt = pe + 0x18
    magic = struct.unpack_from('<H', data, opt)[0]
    assert magic == 0x20b, "PE32+ only"
    imp_rva, imp_sz = struct.unpack_from('<II', data, opt + 112 + 8)
    dly_rva, _ = struct.unpack_from('<II', data, opt + 112 + 13 * 8)
    out = []
    off = rva2off(data, imp_rva)
    while off and off + 20 <= len(data):
        oft, ts, fc, name_rva, iat = struct.unpack_from('<IIIII', data, off)
        if name_rva == 0:
            break
        noff = rva2off(data, name_rva)
        dll = data[noff:data.index(b'\0', noff)].decode('ascii', 'replace')
        io = rva2off(data, iat if iat else oft)
        n = 0
        while io:
            t = struct.unpack_from('<Q', data, io + n * 8)[0]
            if t == 0:
                break
            if t & (1 << 63):
                out.append((dll, '#%d' % (t & 0xffff)))
            else:
                toff = rva2off(data, t & 0x7fffffff)
                sym = data[toff + 2:data.index(b'\0', toff + 2)].decode('ascii', 'replace')
                out.append((dll, sym))
            n += 1
        off += 20
    # delay imports
    doff = rva2off(data, dly_rva)
    if doff:
        while doff + 32 <= len(data):
            attrs, handle, vtable, addr, name_rva, tname, ifn, its = \
                struct.unpack_from('<8I', data, doff)
            if name_rva == 0:
                break
            if attrs == 0:
                noff = rva2off(data, name_rva)
                dll = data[noff:data.index(b'\0', noff)].decode('ascii', 'replace')
                io = rva2off(data, ifn)
                n = 0
                while io:
                    t = struct.unpack_from('<I', data, io + n * 4)[0]
                    if t == 0:
                        break
                    if t & (1 << 31):
                        out.append((dll, '#%d' % (t & 0x7fff)))
                    else:
                        toff = rva2off(data, t)
                        sym = data[toff + 2:data.index(b'\0', toff + 2)].decode('ascii', 'replace')
                        out.append((dll, sym))
                    n += 1
            doff += 32
    return out

# DLL -> bridge file name(s) that implement it (many DLLs share a bridge)
DLL_ALIASES = {
    'advapi32': ['reg', 'kernel32', 'kernel32_gap'],
    'kernelbase': ['kernel32', 'kernel32_gap', 'mem', 'thread', 'sys', 'sys_extras'],
    'd3d11': ['d3d'],
    'dxgi': ['d3d'],
    'd3dcompiler_43': ['d3d'],
    'd3dcompiler_47': ['d3d'],
    'd3dx11_43': ['d3dx'],
    'd3dx9_43': ['d3dx9'],
    'oleaut32': ['ole'],
    'ws2_32': ['ws2'],
    'wsock32': ['ws2'],
    'mswsock': ['ws2'],
    'xinput1_3': ['xinput'],
    'dinput8': ['dinput'],
    'dinput': ['dinput'],
    'ole32': ['ole'],
    'steam_api64': ['steam'],
    'steam_api': ['steam'],
    'fmodex64': ['fmod'],
    'fmodex': ['fmod'],
    'fmod_event64': ['fmod'],
    'fmod_event': ['fmod'],
    'amd_ags_x64': ['nvapi'],
    'dstorage': ['dstorage'],
    'dstoragecore': ['dstorage'],
    'bink2w64': ['bink'],
    'binkw64': ['bink'],
    'bink2w32': ['bink'],
    'oo2core_6_win64': ['oodle'],
    'oo2core_8_win64': ['oodle'],
    'oo2core_5_win64': ['oodle'],
    'xinput1_4': ['xinput'],
    'bcrypt': ['crypt32'],
    'ncrypt': ['crypt32'],
    'wldap32': ['wldap32'],
    'imm32': ['imm32'],
    'shlwapi': ['shlwapi'],
    'shell32': ['shell'],
    'shfolder': ['shell'],
    'comctl32': ['comctl'],
    'comdlg32': ['sys_extras'],
    'api-ms-win-core-synch-l1-2-0': ['kernel32', 'thread'],
    'api-ms-win-crt-convert-l1-1-0': ['msvcrt'],
    'api-ms-win-crt-environment-l1-1-0': ['msvcrt'],
    'api-ms-win-crt-heap-l1-1-0': ['msvcrt'],
    'api-ms-win-crt-locale-l1-1-0': ['msvcrt'],
    'api-ms-win-crt-math-l1-1-0': ['msvcrt'],
    'api-ms-win-crt-runtime-l1-1-0': ['msvcrt'],
    'api-ms-win-crt-stdio-l1-1-0': ['msvcrt'],
    'api-ms-win-crt-string-l1-1-0': ['msvcrt'],
    'api-ms-win-crt-time-l1-1-0': ['msvcrt'],
    'api-ms-win-crt-filesystem-l1-1-0': ['msvcrt'],
    'api-ms-win-crt-process-l1-1-0': ['msvcrt'],
    'api-ms-win-core-console-l1-1-0': ['kernel32'],
    'api-ms-win-core-debug-l1-1-0': ['kernel32'],
    'api-ms-win-core-errorhandling-l1-1-0': ['kernel32'],
    'api-ms-win-core-file-l1-1-0': ['kernel32'],
    'api-ms-win-core-handle-l1-1-0': ['kernel32'],
    'api-ms-win-core-heap-l1-1-0': ['kernel32'],
    'api-ms-win-core-interlocked-l1-1-0': ['kernel32'],
    'api-ms-win-core-libraryloader-l1-1-0': ['kernel32'],
    'api-ms-win-core-localization-l1-2-0': ['kernel32'],
    'api-ms-win-core-memory-l1-1-0': ['kernel32'],
    'api-ms-win-core-namedpipe-l1-1-0': ['kernel32'],
    'api-ms-win-core-processenvironment-l1-1-0': ['kernel32'],
    'api-ms-win-core-processthreads-l1-1-0': ['kernel32'],
    'api-ms-win-core-profile-l1-1-0': ['kernel32'],
    'api-ms-win-core-rtlsupport-l1-1-0': ['ntdll'],
    'api-ms-win-core-string-l1-1-0': ['kernel32'],
    'api-ms-win-core-sysinfo-l1-1-0': ['kernel32'],
    'api-ms-win-core-timezone-l1-1-0': ['kernel32'],
    'api-ms-win-core-util-l1-1-0': ['kernel32'],
    'api-ms-win-crt-private-l1-1-0': ['msvcrt'],
    'api-ms-win-core-libraryloader-l1-2-0': ['kernel32'],
    'api-ms-win-core-processthreads-l1-1-1': ['kernel32'],
    'libgcc_s_seh-1': [],
    'ucrtbase': ['msvcrt'],
    'steam_api64': ['steam_api'],
    'steam_api': ['steam_api'],
    'avrt': ['wasapi'],
    'dwmapi': ['d3d'],
    'uxtheme': ['sys_extras'],
    'powrprof': ['sys_extras'],
    'pdh': ['sys_extras'],
    'wer': ['sys_extras'],
    'userenv': ['sys_extras'],
    'wtsapi32': ['sys_extras'],
    'propsys': ['sys_extras'],
    'version': ['winrt'],
    'shcore': ['winrt'],
    'dwrite': ['winrt'],
    'uiautomationcore': ['winrt'],
    'combase': ['winrt', 'ole'],
    'ntdll': ['ntdll'],
    'gdi32': ['gdi'],
    'gdiplus': ['gdiplus'],
    'user32': ['user32', 'user32_gap'],
    'kernel32': ['kernel32', 'kernel32_gap', 'mem', 'thread', 'sys', 'res'],
}

def build_impl_set(src_dir):
    """Collect function names mentioned in bridge sources."""
    impl = set()
    dlls = set()
    for fn in os.listdir(src_dir):
        if fn.startswith('bridge_') and fn.endswith('.cpp'):
            dlls.add(fn[len('bridge_'):-len('.cpp')])
        path = os.path.join(src_dir, fn)
        if not os.path.isfile(path):
            continue
        try:
            text = open(path, 'r', errors='replace').read()
        except Exception:
            continue
        for m in re.finditer(r'"([A-Za-z_][A-Za-z0-9_]*)"', text):
            impl.add(m.group(1))
    return impl, dlls, DLL_ALIASES

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe')
    ap.add_argument('--src', default='src/trt')
    ap.add_argument('--out', default=None)
    ap.add_argument('--scaffold', default=None,
                    help='directory to write stub bridge .cpp/.h skeletons for missing DLLs')
    args = ap.parse_args()

    # Detect 32-bit PE
    import struct
    with open(args.exe, 'rb') as f:
        f.seek(0x3C)
        pe_off = struct.unpack('<I', f.read(4))[0]
        f.seek(pe_off)
        magic = f.read(2)
        if magic == b'PE':
            f.seek(pe_off + 4)
            machine = struct.unpack('<H', f.read(2))[0]
            if machine == 0x014c:
                print('WARNING: 32-bit (x86) PE detected. Tiramisu cannot execute 32-bit code.')
                print('         Only 64-bit (x86-64) games are supported.')
                print()
            elif machine == 0x8664:
                print('INFO: 64-bit (x86-64) PE detected. OK.')
                print()
    # Detect anti-cheat DLLs
    ac_dlls = {'easyanticheat', 'battleye', 'vanguard', 'punkbuster',
               'xigncode', 'nprotect', 'fairfight', 'vac'}
    imps = imports(args.exe)
    found_ac = [d[0] for d in imps if any(ac in d[0].lower() for ac in ac_dlls)]
    found_ac = sorted(set(found_ac))
    if found_ac:
        print('WARNING: Anti-cheat DLLs detected: %s' % ', '.join(found_ac))
        print('         Multiplayer will not work. Single-player may work if anti-cheat is optional.')
        print()
    impl, impl_dlls, aliases = build_impl_set(args.src)

    by_dll = {}
    for dll, sym in imps:
        by_dll.setdefault(dll.lower(), []).append(sym)

    report = {}
    total = 0
    covered = 0
    missing_dlls = []
    for dll in sorted(by_dll):
        syms = by_dll[dll]
        # dll -> bridge name mapping
        bridge_name = dll.replace('.dll', '').replace('.ocx', '')
        alias_list = aliases.get(bridge_name, [bridge_name])
        has_bridge = any(a in impl_dlls for a in alias_list)
        miss = []
        hit = []
        for s in syms:
            if s in impl:
                hit.append(s)
            else:
                miss.append(s)
        total += len(syms)
        covered += len(hit)
        report[dll] = {
            'imports': len(syms),
            'implemented': len(hit),
            'missing': len(miss),
            'has_bridge': has_bridge,
            'missing_symbols': sorted(set(miss)),
        }
        if not has_bridge:
            missing_dlls.append(dll)

    # print
    print('=== auto-compat report: %s ===' % args.exe)
    print('total imports: %d   implemented: %d   missing: %d   (%.1f%%)' % (
        total, covered, total - covered, 100.0 * covered / total if total else 0))
    print()
    print('%-28s %6s %6s %6s %s' % ('DLL', 'imp', 'impl', 'miss', 'bridge'))
    for dll in sorted(report):
        r = report[dll]
        flag = 'OK' if r['has_bridge'] else 'NO BRIDGE'
        print('%-28s %6d %6d %6d %s' % (dll, r['imports'], r['implemented'], r['missing'], flag))
    if missing_dlls:
        print()
        print('DLLs with no bridge at all:')
        for d in missing_dlls:
            print('  - %s (%d imports)' % (d, report[d]['imports']))
    # top missing symbols
    all_missing = []
    for dll in report:
        for s in report[dll]['missing_symbols']:
            all_missing.append((dll, s))
    if all_missing:
        print()
        print('Top missing symbols (first 40):')
        for dll, s in all_missing[:40]:
            print('  %s!%s' % (dll, s))

    if args.out:
        with open(args.out, 'w') as f:
            json.dump({'exe': args.exe, 'total': total, 'covered': covered,
                       'report': report}, f, indent=2)
        print()
        print('wrote %s' % args.out)

    if args.scaffold:
        os.makedirs(args.scaffold, exist_ok=True)
        for dll in missing_dlls:
            bridge_name = dll.replace('.dll', '').replace('.ocx', '').replace('-', '_')
            cpp_path = os.path.join(args.scaffold, 'bridge_%s.cpp' % bridge_name)
            h_path = os.path.join(args.scaffold, 'bridge_%s.h' % bridge_name)
            syms = [s for s in report[dll]['missing_symbols']
                    if re.match(r'^[A-Za-z_][A-Za-z0-9_]*$', s)]
            if not syms:
                print('skip %s (only mangled symbols)' % dll)
                continue
            with open(cpp_path, 'w') as f:
                f.write('// Auto-scaffolded bridge for %s\n' % dll)
                f.write('// Generated by auto-compat.py --scaffold\n')
                f.write('#include "bridge_%s.h"\n' % bridge_name)
                f.write('#include "thunk.h"\n#include "trt.h"\n')
                f.write('#include <stdio.h>\n\n')
                f.write('namespace {\nnamespace log { static void line(const char* s) { trt_core_log(s); } }\n')
                f.write('#define log_line(s_) log::line(s_)\n}\n\nextern "C" {\n\n')
                for s in syms:
                    f.write('// TODO: implement %s\n' % s)
                    f.write('void* TRT_GUEST_ABI %s_%s(void) {\n' % (bridge_name, s))
                    f.write('    log_line("%s: %s -> stub");\n    return nullptr;\n}\n\n' % (bridge_name, s))
                f.write('} // extern "C"\n\n')
                f.write('extern "C" int trt_bridge_register_%s(void) {\n' % bridge_name)
                f.write('    static const trt_named_fn k_fns[] = {\n')
                for s in syms:
                    f.write('        {"%s", (void*)&%s_%s},\n' % (s, bridge_name, s))
                f.write('    };\n')
                f.write('    trt_module_register("%s", TRT_ROUTE_BUILTIN, k_fns,\n' % dll)
                f.write('                        (int)(sizeof(k_fns) / sizeof(k_fns[0])));\n')
                f.write('    return 0;\n}\n')
            with open(h_path, 'w') as f:
                f.write('#ifndef TRT_BRIDGE_%s_H\n#define TRT_BRIDGE_%s_H\n' % (bridge_name.upper(), bridge_name.upper()))
                f.write('extern "C" int trt_bridge_register_%s(void);\n' % bridge_name)
                f.write('#endif\n')
            print('scaffolded %s -> %s' % (dll, cpp_path))

if __name__ == '__main__':
    main()
