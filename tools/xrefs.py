import struct, sys
path = r"C:\Program Files (x86)\Steam\steamapps\common\MountBlade Warband\mb_warband_wse2_x64.exe"
data = open(path, 'rb').read()
pe = struct.unpack_from('<I', data, 0x3c)[0]
nsec = struct.unpack_from('<H', data, pe + 6)[0]
optsz = struct.unpack_from('<H', data, pe + 20)[0]
base = struct.unpack_from('<Q', data, pe + 24 + 24)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    name = data[o:o+8].rstrip(b'\0').decode()
    vsize, va, rsize, rptr = struct.unpack_from('<IIII', data, o + 8)
    secs.append((name, va, vsize, rptr, rsize))
targets = [int(t, 16) for t in sys.argv[1:]]
for name, va, vsize, rptr, rsize in secs:
    if name != '.text':
        continue
    for i in range(rptr, rptr + rsize - 5):
        if data[i] == 0xE8 or data[i] == 0xE9:
            rel = struct.unpack_from('<i', data, i + 1)[0]
            src = base + va + (i - rptr)
            dst = src + 5 + rel
            if dst in targets:
                print(hex(dst), 'call' if data[i] == 0xE8 else 'jmp', hex(src))
