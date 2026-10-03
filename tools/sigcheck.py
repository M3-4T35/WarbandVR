import struct, sys, re
path = r"C:\Program Files (x86)\Steam\steamapps\common\MountBlade Warband\mb_warband_wse2_x64.exe"
data = open(path, 'rb').read()
pe = struct.unpack_from('<I', data, 0x3c)[0]
nsec = struct.unpack_from('<H', data, pe + 6)[0]
optsz = struct.unpack_from('<H', data, pe + 20)[0]
base = struct.unpack_from('<Q', data, pe + 24 + 24)[0]
text = None
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    if data[o:o+8].rstrip(b'\0') == b'.text':
        vsize, va, rsize, rptr = struct.unpack_from('<IIII', data, o + 8)
        text = (va, rptr, rsize)
va, rptr, rsize = text
blob = data[rptr:rptr+rsize]
def check(name, sig):
    parts = sig.split()
    rx = b''.join(b'.' if p == '??' else re.escape(bytes([int(p, 16)])) for p in parts)
    hits = [base + va + m.start() for m in re.finditer(rx, blob, re.S)]
    print(f"{name}: {len(hits)} occurrence(s) {[hex(h) for h in hits[:5]]}")
check(sys.argv[1], sys.argv[2])
