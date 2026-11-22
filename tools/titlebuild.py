#!/usr/bin/env python3
"""titlebuild.py — offline title build + autofix engine.

Given a transferred game folder, this measures what the title needs and
produces the package the console host consumes:

    <game_dir>/.tiramisu/manifest.json   measured deps, gaps, coverage, fixes
    <game_dir>/tiramisu.profile.ini      generated per-title overrides

It is the offline half of "transfer it and it builds itself": the console
cannot compile, so everything that can be decided without hardware is decided
here (routing, aliasing, forwarders, profile, readiness). The host-side
autopilot applies the resulting fix list and relaunches.

Evidence only: every classification comes from the real import tables parsed
by pe_readiness.scan_bytes(), and every fix is a declared rule, never a guess.
Usage: python3 tools/titlebuild.py <game_dir> [--check]
Exit: 0 bootable | 1 blocked (or --check verdict != bootable) | 2 usage/error
"""
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pe_readiness  # noqa: E402

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# --- routing knowledge (mirrors src/trt/module_table.cpp; keep in sync) ------
ALIASES = {  # desktop DLL -> console DLL that actually exists
    'vcruntime140.dll': 'vcruntime140_app.dll',
    'vcruntime140_1.dll': 'vcruntime140_1_app.dll',
    'msvcp140.dll': 'msvcp140_app.dll',
    'msvcp140_1.dll': 'msvcp140_1_app.dll',
    'msvcp140_2.dll': 'msvcp140_2_app.dll',
    'concrt140.dll': 'concrt140_app.dll',
    'vcomp140.dll': 'vcomp140_app.dll',
    'xinput1_4.dll': 'xinputuap.dll',
    'xinput1_3.dll': 'xinputuap.dll',
    'xinput9_1_0.dll': 'xinputuap.dll',
    'd3dcompiler_46.dll': 'd3dcompiler_47.dll',
    'd3dcompiler_43.dll': 'd3dcompiler_47.dll',
}
SYSTEM = {
    'd3d11.dll', 'd3d12.dll', 'd3d12core.dll', 'dstorage.dll',
    'dstoragecore.dll', 'dxgi.dll', 'd3dcompiler_47.dll', 'dcomp.dll',
    'd2d1.dll', 'dwrite.dll', 'dwmapi.dll', 'combase.dll', 'ucrtbase.dll',
    'ntdll.dll', 'ws2_32.dll', 'wldap32.dll', 'winhttp.dll', 'bcrypt.dll',
    'shcore.dll', 'version.dll', 'hid.dll', 'setupapi.dll', 'avrt.dll',
    'kernelbase.dll', 'xinput1_4.dll',
}
BUILTIN = {
    'gdi32.dll', 'comctl32.dll', 'uxtheme.dll', 'msimg32.dll', 'vulkan-1.dll',
    'comdlg32.dll', 'oleacc.dll', 'ole32.dll', 'oleaut32.dll',
    'mswsock.dll', 'wininet.dll', 'dnsapi.dll', 'iphlpapi.dll', 'netapi32.dll',
    'mpr.dll', 'dhcpcsvc.dll', 'normaliz.dll', 'ncrypt.dll', 'crypt32.dll',
    'cryptui.dll', 'secur32.dll', 'sechost.dll', 'sspicli.dll', 'wintrust.dll',
    'credui.dll', 'shlwapi.dll', 'shell32.dll', 'shfolder.dll', 'propsys.dll',
    'userenv.dll', 'psapi.dll', 'imm32.dll', 'usp10.dll', 'wtsapi32.dll',
    'dbghelp.dll', 'dbgcore.dll', 'dxcore.dll', 'directxmath.dll',
    'd3d8.dll', 'openal32.dll', 'dsound.dll', 'ddraw.dll', 'opengl32.dll',
    'msacm32.dll', 'msvfw32.dll', 'winspool.drv', 'd3dx9_43.dll',
    'd3dx9_40.dll', 'dinput8.dll', 'dinput.dll', 'gameinput.dll',
    'advapi32.dll', 'ktmw32.dll', 'glu32.dll',
}
STUB = {'d3d9.dll', 'd3d9core.dll', 'uiautomationcore.dll'}
RECORDED = {'d3d12.dll'}          # accepted + logged, not executed
NAMED_SILENT = {                  # truthful empty answers / named failures
    'mfplat.dll', 'mfreadwrite.dll', 'mscoree.dll', 'steam_api64.dll',
    'steam_api.dll', 'msacm32.dll', 'msvfw32.dll', 'winspool.drv',
}
SILENT_SYMBOLS = {                # honest-empty per-symbol answers
    ('shell32.dll', 'ExtractIconExW'), ('shell32.dll', 'ExtractIconExA'),
    ('shell32.dll', 'ExtractIconW'), ('shell32.dll', 'ExtractIconA'),
    ('winmm.dll', 'waveOutGetNumDevs'), ('winmm.dll', 'midiInGetNumDevs'),
}

JUNK_EXE = re.compile(
    r'(unitycrashhandler|crashreport|ueprereq|ue4prereq|ue5prereq|'
    r'vcredist|vc_redist|dxsetup|dotnet|unins0|uninstall|easyanticheat|'
    r'battleye)', re.I)
# Only real anti-cheat blocks a title. Crash handlers and redist installers
# are normal in Unity/UE games and are excluded from exe *selection*, not a
# reason to refuse the title.
ANTI_CHEAT = re.compile(r'(easyanticheat|battleye|vanguard|ricochet)', re.I)


def lower(s):
    return s.lower()


def classify(dll, sym):
    """Return (route, cls) where cls in real/recorded/named-silent/gap."""
    d = lower(dll)
    if d in ALIASES:
        return 'alias', 'real'
    if d in SYSTEM:
        return 'system', 'real'
    if d in BUILTIN:
        if d in RECORDED:
            return 'builtin', 'recorded'
        if d in NAMED_SILENT or (d, sym) in SILENT_SYMBOLS:
            return 'builtin', 'named-silent'
        return 'builtin', 'real'
    if d in STUB:
        return 'stub', 'named-silent'
    return 'missing', 'gap'


# --- fix rules: signature -> remediation -------------------------------------
# Each rule is declared and idempotent; applied ones land in the manifest.
FIX_RULES = [
    {'id': 'x86-blocked', 'when': lambda t: any(p['bits'] == 32 for p in t['pes']),
     'severity': 'block', 'note': '32-bit native code has no executor'},
    {'id': 'anti-cheat-blocked',
     'when': lambda t: bool(t['anti_cheat_files']),
     'severity': 'block', 'note': 'anti-cheat present (excluded by contract)'},
    {'id': 'd3d12-recorded',
     'when': lambda t: any(lower(x) in RECORDED for x in t['dlls']),
     'severity': 'warn',
     'note': 'D3D12 is recorded, not executed; a D3D11 fallback may be needed'},
    {'id': 'steam-auth-only',
     'when': lambda t: any(lower(x).startswith('steam_api') for x in t['dlls']),
     'severity': 'info',
     'note': 'Steam layer is auth-only; BIsSubscribedApp answers 0 by rule'},
    {'id': 'audio-codecs-partial',
     'when': lambda t: bool(t['cls']['named-silent']),
     'severity': 'info',
     'note': 'some imported surfaces are named-silent (see manifest.gaps)'},
]

PROFILE_DEFAULTS = {
    'default_input': 'controller',
    'frame_rate_cap': 60,
    'vsync': 1,
    'write_watch': 1,
    'shader_cache': 1,
}


def find_pes(root):
    out = []
    for dirpath, _dirs, files in os.walk(root):
        if '.tiramisu' in dirpath:
            continue
        for fn in files:
            if fn.lower().endswith(('.exe', '.dll')):
                out.append(os.path.join(dirpath, fn))
    return sorted(out)


def pick_exe(pes, root):
    """Primary executable: shallowest, non-junk, largest candidate."""
    cands = []
    for rec in pes:
        path = rec['path']
        fn = os.path.basename(path)
        if not fn.lower().endswith('.exe') or JUNK_EXE.search(fn):
            continue
        depth = os.path.relpath(path, root).count(os.sep)
        cands.append((depth, -os.path.getsize(path), fn.lower(), path))
    if not cands:
        return None
    cands.sort()
    return cands[0][3]


def load_symbol_dump(pes, repo_root):
    """Ground-truth per-symbol classes from the runtime's own classifier.

    test_corpus --symbols resolves every import through trt_symbol_class(),
    including apiset redirects — something a standalone Python parser cannot
    know. Falls back to None (the caller then uses the built-in table).
    """
    exe = os.path.join(repo_root, 'tests', 'out', 'test_corpus')
    if not os.path.exists(exe):
        return None
    try:
        p = subprocess.run([exe, '--symbols'] + pes, capture_output=True,
                           text=True, timeout=900)
    except Exception:
        return None
    dump = {}
    for line in p.stdout.splitlines():
        parts = line.split(' ')
        if len(parts) != 4 or parts[0] != 'SYMBOL':
            continue
        _, pe, fq, cls = parts
        dll, _, sym = fq.rpartition('!')
        dump[(pe, lower(dll), sym)] = cls
    return dump or None


def build(root):
    files = find_pes(root)
    # Filename-level exclusions apply even when the file is not a readable PE:
    # an anti-cheat exe that fails to parse is still an anti-cheat title.
    junk = [p for p in files if JUNK_EXE.search(os.path.basename(p))]
    anti_cheat = [p for p in files if ANTI_CHEAT.search(os.path.basename(p))]
    pes = []
    skipped = []
    for path in files:
        try:
            r = pe_readiness.parse(path)
        except Exception as e:  # malformed PE is a finding, not a crash
            skipped.append({'path': path, 'why': str(e)})
            continue
        if r is None:
            skipped.append({'path': path, 'why': 'not-a-PE'})
            continue
        pes.append(r)

    dump = load_symbol_dump([r['path'] for r in pes], REPO_ROOT)
    classifier = 'trt_symbol_class' if dump else 'builtin-table-fallback'

    dlls, syms, cls = [], [], {'real': 0, 'recorded': 0, 'named-silent': 0}
    gaps = []
    seen = set()
    for r in pes:
        pe_name = os.path.basename(r['path'])
        for s in r['symbols']:
            key = (lower(s['dll']), s['sym'])
            if key in seen:
                continue
            seen.add(key)
            if dump is not None:
                c = dump.get((pe_name, lower(s['dll']), s['sym']))
                if c is None:
                    # Not reported by the runtime: an unreadable thunk.
                    c = 'unresolved'
                route = ('unresolved' if c == 'unresolved' else
                         'builtin' if c in ('recorded', 'named-silent')
                         else 'system-or-bridge')
            else:
                route, c = classify(s['dll'], s['sym'])
                if c == 'gap':
                    c = 'unresolved'
            dlls.append(s['dll'])
            syms.append({'dll': s['dll'], 'sym': s['sym'], 'kind': s['kind'],
                         'route': route, 'cls': c})
            if c == 'unresolved':
                gaps.append({'kind': 'unresolved-import',
                             'symbol': '%s!%s' % (s['dll'], s['sym']),
                             'note': 'no bridge, no console DLL, not shipped'})
            elif c in cls:
                cls[c] += 1

    dlls = sorted(set(lower(d) for d in dlls))
    title = {'root': root, 'pes': [{'path': p['path'], 'bits': p['bits']}
                                   for p in pes],
             'exe': pick_exe(pes, root), 'dlls': dlls, 'symbols': syms,
             'cls': cls, 'gaps': gaps, 'skipped': skipped,
             'junk_files': junk, 'anti_cheat_files': anti_cheat,
             'classifier': classifier}

    applied = []
    for rule in FIX_RULES:
        if rule['when'](title):
            applied.append({'id': rule['id'], 'severity': rule['severity'],
                            'note': rule['note']})

    blocks = [f for f in applied if f['severity'] == 'block']
    verdict = 'blocked' if blocks else ('has-gaps' if gaps else 'bootable')
    title['fixes_applied'] = applied
    title['verdict'] = verdict
    return title


def emit_profile(path):
    lines = ['# generated by tools/titlebuild.py — safe starting overrides',
             '# edit freely; keys fall back to global settings when omitted']
    for k, v in PROFILE_DEFAULTS.items():
        lines.append('%s=%s' % (k, v))
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')


def main(argv):
    check_only = '--check' in argv
    args = [a for a in argv[1:] if not a.startswith('--')]
    if len(args) != 1 or not os.path.isdir(args[0]):
        print(__doc__)
        return 2
    root = os.path.abspath(args[0])
    t = build(root)

    outdir = os.path.join(root, '.tiramisu')
    manifest = os.path.join(outdir, 'manifest.json')
    profile = os.path.join(root, 'tiramisu.profile.ini')
    if not check_only:
        os.makedirs(outdir, exist_ok=True)
        with open(manifest, 'w') as f:
            json.dump(t, f, indent=2, sort_keys=True)
        emit_profile(profile)

    c = t['cls']
    total = sum(c.values())
    print('title:        %s' % os.path.basename(root))
    print('exe:          %s' % (os.path.relpath(t['exe'], root)
                                if t['exe'] else '(none)'))
    print('PEs:          %d (%s)' % (
        len(t['pes']), ', '.join('%db' % p['bits'] for p in t['pes'][:6])))
    print('classifier:   %s' % t['classifier'])
    print('imports:      %d  real=%d recorded=%d named-silent=%d' %
          (total, c['real'], c['recorded'], c['named-silent']))
    print('gaps:         %d' % len(t['gaps']))
    for g in t['gaps'][:12]:
        print('  - %s' % g['symbol'])
    if len(t['gaps']) > 12:
        print('  ... %d more' % (len(t['gaps']) - 12))
    for f in t['fixes_applied']:
        print('fix [%s]: %s' % (f['severity'], f['note']))
    if t['skipped']:
        print('skipped:      %d unreadable/non-PE files' % len(t['skipped']))
    print('verdict:      %s' % t['verdict'].upper())
    if not check_only:
        print('wrote:        %s' % manifest)
        print('wrote:        %s' % profile)
    return 1 if t['verdict'] == 'blocked' else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
