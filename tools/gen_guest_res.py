#!/usr/bin/env python3
# tools/gen_guest_res.py
# Emits the .rsrc section bytes for the resource-test guest EXE as a
# NASM-free GNU-as assembly object, then (second pass) patches the three
# IMAGE_RESOURCE_DATA_ENTRY DataRva fields with the absolute image RVAs
# once the linker has chosen the .rsrc section RVA.
#
# Layout (section-relative):
#   0x000 type dir (RT_RCDATA=10 -> dir@0x40, RT_STRING=6 -> dir@0x78)
#   0x040 name dir RCDATA (id 101 -> dir@0x90, name "PAYLOAD" -> dir@0xA0)
#   0x078 name dir STRING (id 6 -> dir@0xB0)
#   0x090 lang dir (0x409 -> DATA@0x1C0)
#   0x0A0 lang dir (0x409 -> DATA@0x1D0)
#   0x0B0 lang dir (0x409 -> DATA@0x1E0)
#   0x1A0 unicode name "PAYLOAD"
#   0x1C0/0x1D0/0x1E0 DATA_ENTRYs (DataRva = placeholder 0x11223344)
#   0x2C0 RCDATA 101 blob (16 bytes)
#   0x2D0 PAYLOAD blob (4 bytes)
#   0x2E0 string block (16 slots, slot 5 = "HELLO RSRC")
import struct, sys, subprocess, os

SECTION_SIZE = 0x340
PLACEHOLDER = 0x11223344

def dir_header(named, ided):
    # Characteristics, TimeDateStamp, Major/MinorVersion = 0
    return struct.pack('<IIHH', 0, 0, 0, 0) + struct.pack('<HH', named, ided)

def ent_id(key, off_to_dir):
    return struct.pack('<II', key, 0x80000000 | off_to_dir)

def ent_name(str_off, off_to_dir):
    return struct.pack('<II', 0x80000000 | str_off, 0x80000000 | off_to_dir)

def data_entry(daterva_placeholder, size):
    return struct.pack('<IIHHI', daterva_placeholder, size, 0, 0, 0)

def ustr(s):
    b = s.encode('utf-16-le')
    return struct.pack('<HH', len(b), 0) + b

def build(section_rva):
    sec = bytearray(SECTION_SIZE)
    def put(off, blob):
        sec[off:off+len(blob)] = blob
    # type level @0
    put(0x00, dir_header(0, 2))
    put(0x10, ent_id(10, 0x40))
    put(0x18, ent_id(6, 0x78))
    # name level RCDATA @0x40
    put(0x40, dir_header(1, 1))
    put(0x50, ent_id(101, 0x90))
    put(0x58, ent_name(0x1A0, 0xA0))
    # name level STRING @0x78
    put(0x78, dir_header(0, 1))
    put(0x88, ent_id(6, 0xB0))
    # lang level RCDATA id 101 @0x90
    put(0x90, dir_header(0, 1))
    put(0xA0 - 0x10, b'')  # noop guard
    put(0xA0, dir_header(0, 1))
    # 0x90 dir entry at 0xA0? NO: 0x90 dir header is 16 bytes, entry at 0xA0.
    # fix: entry of dir@0x90 goes at 0xA0... that IS dir@0xA0's slot. Shift:
    return sec

# The 16-byte directory header leaves the entry at header+16. Offsets:
#   dir@0x90 -> entry@0xA0 (conflicts with dir@0xA0). Re-layout:
#   0x090 lang dir RCDATA#101, entry @0x0A0 -> DATA@0x1C0
#   0x0A8 lang dir RCDATA"PAYLOAD" header, entry @0x0B8 -> DATA@0x1D0
#   0x0C0 lang dir STRING#6 header, entry @0x0D0 -> DATA@0x1E0
def build2(section_rva):
    sec = bytearray(SECTION_SIZE)
    def put(off, blob):
        sec[off:off+len(blob)] = blob
    put(0x000, dir_header(0, 2))
    put(0x010, ent_id(10, 0x40))
    put(0x018, ent_id(6, 0x78))
    put(0x040, dir_header(1, 1))
    put(0x050, ent_id(101, 0x90))
    put(0x058, ent_name(0x1A0, 0xA8))
    put(0x078, dir_header(0, 1))
    put(0x088, ent_id(1, 0xC0))  # string block for ids 0..15 is NAME 1
    # lang dirs (16-byte header + one 8-byte entry)
    put(0x090, dir_header(0, 1))
    put(0x0A0, struct.pack('<II', 0x409, 0x1C0))
    put(0x0A8, dir_header(0, 1))
    put(0x0B8, struct.pack('<II', 0x409, 0x1D0))
    put(0x0C0, dir_header(0, 1))
    put(0x0D0, struct.pack('<II', 0x409, 0x1E0))
    # unicode name string
    put(0x1A0, ustr("PAYLOAD"))
    # data entries with placeholders (patched after link)
    put(0x1C0, data_entry(PLACEHOLDER, 16))
    put(0x1D0, data_entry(PLACEHOLDER, 4))
    put(0x1E0, data_entry(PLACEHOLDER, 64))
    # blobs
    put(0x2C0, bytes([0xDE,0xAD,0xBE,0xEF,0x01,0x02,0x03,0x04,
                      0xAA,0xBB,0xCC,0xDD,0x55,0x66,0x77,0x88]))
    put(0x2D0, bytes([0xCA,0xFE,0xBA,0xBE]))
    # string block: 16 slots; slot 5 = "HELLO RSRC"
    sb = b''
    for i in range(16):
        if i == 5:
            t = "HELLO RSRC"
            sb += struct.pack('<H', len(t)) + t.encode('utf-16-le')
        else:
            sb += struct.pack('<H', 0)
    put(0x2E0, sb)
    return bytes(sec)

def emit_asm(path, blob):
    with open(path, 'w') as f:
        f.write('.section .rsrc,"a"\n')
        f.write('.balign 1\n')
        f.write('.globl g_rsrc\n')
        f.write('g_rsrc:\n')
        for i in range(0, len(blob), 16):
            chunk = blob[i:i+16]
            f.write('.byte ' + ','.join(str(b) for b in chunk) + '\n')
        f.write('.section .drectve,"a"\n')
        f.write('.ascii ""\n')

def main():
    out_asm = sys.argv[1] if len(sys.argv) > 1 else 'tests/out/guest_res_rsrc.s'
    out_obj = sys.argv[2] if len(sys.argv) > 2 else 'tests/out/guest_res_rsrc.obj'
    # pass 1: placeholders
    sec = build2(0)
    emit_asm(out_asm, sec)
    r = subprocess.run(['as', '--64', '-o', out_obj, out_asm])
    if r.returncode != 0:
        sys.exit('as failed')
    print(f'wrote {out_obj} (placeholders in place, {SECTION_SIZE} bytes)')

if __name__ == '__main__':
    main()
