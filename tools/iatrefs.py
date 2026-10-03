import struct, sys
path = r"C:\Program Files (x86)\Steam\steamapps\common\MountBlade Warband\mb_warband_wse2_x64.exe"
data = open(path, 'rb').read()
pe = struct.unpack_from('<I', data, 0x3c)[0]
nsec = struct.unpack_from('<H', data, pe + 6)[0]
optsz = struct.unpack_from('<H', data, pe + 20)[0]
base = struct.unpack_from('<Q', data, pe + 24 + 24)[0]
targets = [int(t, 16) for t in sys.argv[1:]]
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    if data[o:o+8].rstrip(b'\0') != b'.text':
        continue
    vsize, va, rsize, rptr = struct.unpack_from('<IIII', data, o + 8)
    for j in range(rptr, rptr + rsize - 6):
        if data[j] == 0xFF and data[j+1] in (0x15, 0x25):
            disp = struct.unpack_from('<i', data, j + 2)[0]
            src = base + va + (j - rptr)
            if src + 6 + disp in targets:
                print(hex(src + 6 + disp), hex(src))
