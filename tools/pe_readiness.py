#!/usr/bin/env python3
"""pe_readiness.py — measured PE readiness audit for the Tiramisu gate.

Reads real PE headers (no heuristics, no model knowledge) and reports, per file:
  machine 32/64, import DLL count, delay-import presence + descriptor/symbol
  count, bound-import presence, TLS directory presence, and the delay DLL names.

Why: the loader's known v0 limitation is that delay imports are detected but
not processed. This tool measures how much of the real corpus that costs, so
the gap is ranked by evidence instead of guesswork.

Usage: python3 tools/pe_readiness.py <pe> [<pe> ...]
       python3 tools/pe_readiness.py corpus-local/*/*.dll
Exit: 0 always (it is an audit; the caller decides).
"""
import mmap
import struct
import sys


def u16(b, o):
    return struct.unpack_from('<H', b, o)[0]


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def rva_to_off(b, sections, rva):
    # File-based reads must stay inside the RAW data of a section: a section's
    # VirtualSize is often larger than its PointerToRawData extent (glslang.dll
    # in the BotW corpus is one), and reading past EOF is a parse error, not a
    # finding. The loader maps zero-fill for that tail; an audit reads bytes.
    for va, _vsz, rsz, ptr in sections:
        if va <= rva < va + rsz:
            off = ptr + (rva - va)
            return off if off < len(b) else None
    return None


def cstr(b, off):
    end = b.find(b'\0', off)
    return b[off:end].decode('latin-1') if end > off else ''


def parse(path):
    f = open(path, 'rb')
    b = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    if b[:2] != b'MZ':
        return None
    pe = u32(b, 0x3C)
    if b[pe:pe + 4] != b'PE\0\0':
        return None
    coff = pe + 4
    machine = u16(b, coff)
    nsec = u16(b, coff + 2)
    opt_sz = u16(b, coff + 16)
    opt = coff + 20
    magic = u16(b, opt)
    is64 = magic == 0x20B
    dd_off = opt + (112 if is64 else 96)
    sections = []
    image_base = (struct.unpack_from('<Q', b, opt + 24)[0] if is64
                  else u32(b, opt + 28))
    sec = opt + opt_sz
    for i in range(nsec):
        s = sec + i * 40
        sections.append((u32(b, s + 12), u32(b, s + 8), u32(b, s + 16), u32(b, s + 20)))
    dirs = {}
    for idx, name in ((1, 'import'), (13, 'delay'), (11, 'bound')):
        rva, size = u32(b, dd_off + idx * 8), u32(b, dd_off + idx * 8 + 4)
        dirs[name] = (rva, size)

    def dll_names(dir_rva, is_delay=False):
        names, symbols = [], 0
        if not dir_rva:
            return names, 0
        off = rva_to_off(b, sections, dir_rva)
        if off is None:
            return names, -1
        # Bounded walks: a malformed or adversarial PE must not hang the
        # audit (the loader's fuzz suite covers crash safety; this tool
        # only has to terminate).
        for _ in range(4096):
            desc = b[off:off + 32]
            if len(desc) < 32 or desc == b'\0' * 32:
                break
            # Import descriptor: OriginalFirstThunk@0, Name@4, FirstThunk@16.
            # Delay descriptor:   Attributes@0, DllNameRVA@4, IAT@12, INT@16.
            # Both carry the module name at +4; only the thunk field moves.
            # Older linkers store VIRTUAL ADDRESSES in delay descriptors
            # (attributes bit 0 == 0); pe_loader.cpp handles both shapes, so
            # the audit must too, or it undercounts delay symbols.
            shift = 0
            if is_delay and not (u32(desc, 0) & 1):
                shift = image_base
            name_rva = u32(desc, 4)
            if shift and name_rva >= shift:
                name_rva -= shift
            noff = rva_to_off(b, sections, name_rva) if name_rva else None
            names.append(cstr(b, noff) if noff is not None else '?')
            s = u32(desc, 16)  # INT (delay: ImportNameTableRVA)
            if shift and s >= shift:
                s -= shift
            if not s and not is_delay:
                s = u32(desc, 0)  # fall back to OriginalFirstThunk
            soff = rva_to_off(b, sections, s) if s else None
            if soff is not None:
                step = 4 if not is64 else 8
                for _ in range(200000):
                    if soff + step > len(b):
                        break
                    v = u32(b, soff) if not is64 else struct.unpack_from('<Q', b, soff)[0]
                    if v == 0:
                        break
                    if not (v >> (31 if not is64 else 63)):
                        symbols += 1
                    soff += step
            off += 32
        return names, symbols

    imp, imp_syms = dll_names(dirs['import'][0])
    dly, dly_syms = dll_names(dirs['delay'][0], is_delay=True)
    tls = u32(b, dd_off + 9 * 8)
    return {
        'path': path, 'machine': machine,
        'bits': 64 if is64 else 32,
        'imports': len(imp), 'import_syms': imp_syms,
        'delay': len(dly), 'delay_syms': dly_syms, 'delay_dlls': dly,
        'bound': bool(dirs['bound'][0]), 'tls': bool(tls),
    }


def main(argv):
    rows = []
    for p in argv[1:]:
        try:
            r = parse(p)
        except Exception as e:  # a malformed PE is a finding, not a crash
            print('%-46s PARSE-ERROR %s' % (p, e))
            continue
        if r is None:
            # Never drop a file silently: a non-PE on the find list is itself
            # a finding (wrong file staged, or a data file named .dll).
            print('%-46s NOT-A-PE (skipped)' % p.split('/')[-1])
            continue
        rows.append(r)
        print('%-46s %-2d imp=%-3d dly=%-3d(%d syms) %s%s' % (
            p.split('/')[-1], r['bits'], r['imports'], r['delay'],
            r['delay_syms'],
            ('bound ' if r['bound'] else ''),
            ('tls' if r['tls'] else '')))
        if r['delay_dlls']:
            print('    delay: %s' % ', '.join(r['delay_dlls']))
    if not rows:
        print('no PEs')
        return 0
    n32 = sum(1 for r in rows if r['bits'] == 32)
    nd = sum(1 for r in rows if r['delay'])
    ds = sum(r['delay_syms'] for r in rows)
    print('\nSUMMARY: %d PEs | %d PE32 / %d PE32+ | %d with delay imports '
          '(%d delay symbols)' % (len(rows), n32, len(rows) - n32, nd, ds))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
