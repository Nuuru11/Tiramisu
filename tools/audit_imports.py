#!/usr/bin/env python3
# tiramisu audit: enumerate a real PE64's full import surface and check it
# against the runtime's provided (bridged) module!symbol set.
# Read-only on the game file: no execution, no extraction.
import struct, sys

def load(path):
    return open(path, 'rb').read()

def rva2off(d, rva):
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    sec0 = opt = pe + 0x18 + optsz
    for i in range(nsec):
        s = sec0 + i * 40
        vsz, va = struct.unpack_from('<II', d, s + 8)
        rawsz, raw = struct.unpack_from('<II', d, s + 16)
        if va <= rva < va + max(vsz, rawsz):
            return rva - va + raw
    return None

def imports(path):
    d = load(path)
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    opt = pe + 0x18
    magic = struct.unpack_from('<H', d, opt)[0]
    assert magic == 0x20b, "PE32+ only"
    imp_rva, imp_sz = struct.unpack_from('<II', d, opt + 112 + 8)
    # also delay imports (dir index 13)
    dly_rva, dly_sz = struct.unpack_from('<II', d, opt + 112 + 13 * 8)
    out = []
    off = rva2off(d, imp_rva)
    while off and off + 20 <= len(d):
        oft, ts, fc, name_rva, iat = struct.unpack_from('<IIIII', d, off)
        if name_rva == 0:
            break
        noff = rva2off(d, name_rva)
        dll = d[noff:d.index(b'\0', noff)].decode()
        io = rva2off(d, iat if iat else oft)
        n = 0
        while io:
            t = struct.unpack_from('<Q', d, io + n * 8)[0]
            if t == 0:
                break
            if t & (1 << 63):
                out.append((dll, '#%d' % (t & 0xffff), 'iat'))
            else:
                toff = rva2off(d, t & 0x7fffffff)
                sym = d[toff + 2:d.index(b'\0', toff + 2)].decode()
                out.append((dll, sym, 'iat'))
            n += 1
        off += 20
    # delay imports: image load config style, ImgDelayDescr
    doff = rva2off(d, dly_rva)
    if doff:
        while doff + 32 <= len(d):
            attrs, handle, vtable, addr, name_rva, tname, ifn, its = \
                struct.unpack_from('<8I', d, doff)
            if name_rva == 0:
                break
            if attrs == 0:  # RVA form
                noff = rva2off(d, name_rva)
                dll = d[noff:d.index(b'\0', noff)].decode()
                io = rva2off(d, ifn)
            else:  # VA form
                # rare; skip (name would be VA too)
                doff += 32
                continue
            n = 0
            while io:
                t = struct.unpack_from('<I', d, io + n * 4)[0]
                if t == 0:
                    break
                if t & (1 << 31):
                    out.append((dll, '#%d' % (t & 0x7fff), 'delay'))
                else:
                    toff = rva2off(d, t)
                    sym = d[toff + 2:d.index(b'\0', toff + 2)].decode()
                    out.append((dll, sym, 'delay'))
                n += 1
            doff += 32
    return out

if __name__ == '__main__':
    path = sys.argv[1]
    imps = imports(path)
    dlls = sorted(set(d.lower() for d, s, k in imps))
    print('%d imports across %d DLLs' % (len(imps), len(dlls)))
    for dll in dlls:
        syms = [s for d, s, k in imps if d.lower() == dll]
        print('  %-28s %d' % (dll, len(syms)))
