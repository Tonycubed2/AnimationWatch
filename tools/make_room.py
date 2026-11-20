# Builds AnimationWatch.esp (ESL-flagged): one interior cell, the private viewing room for the animation clone.
# GenKit room kit from Skyrim.esm (256 x 256 tiles, 240 high), 5 x 5 tiles = 1280 x 1280, centred on 0,0, floor at z 0.
# Piece orientation read from cell CTest (000B1783): GenKitRmCorIn01 unrotated has walls on -x and +y; rotation z is
# clockwise; GenKitRmWall01 unrotated has its wall on +y (bounds y -128..112).
import struct, math, sys
SKY = 0x00000000
CorIn, Wall, Mid = 0x1305B, 0x13060, 0x1305F
LightShadow = 0xF3A89   # FXLightWhiteBasicShadow, radius 512, white
own = 0x01000800        # first object id of an ESL
recs = []               # (type, formid, subrecords bytes)
def sub(t, data): return t + struct.pack('<H', len(data)) + data
def z(s): return s.encode('latin1') + b'\0'
nid = [0x801]
def ref(base, x, y, zz, rz=0.0):
    fid = 0x01000000 | nid[0]; nid[0] += 1
    body = sub(b'NAME', struct.pack('<I', base)) + sub(b'DATA', struct.pack('<6f', x, y, zz, 0.0, 0.0, rz))
    return (b'REFR', fid, body)
N = 5; T = 256.0; half = (N - 1) / 2
refs = []
deg = math.pi / 2
for i in range(N):
    for j in range(N):
        x = (i - half) * T; y = (j - half) * T
        left, right, bottom, top = i == 0, i == N - 1, j == 0, j == N - 1
        if left and top: refs.append(ref(CorIn, x, y, 0, 0.0))
        elif right and top: refs.append(ref(CorIn, x, y, 0, deg))
        elif right and bottom: refs.append(ref(CorIn, x, y, 0, 2 * deg))
        elif left and bottom: refs.append(ref(CorIn, x, y, 0, 3 * deg))
        elif top: refs.append(ref(Wall, x, y, 0, 0.0))
        elif right: refs.append(ref(Wall, x, y, 0, deg))
        elif bottom: refs.append(ref(Wall, x, y, 0, 2 * deg))
        elif left: refs.append(ref(Wall, x, y, 0, 3 * deg))
        else: refs.append(ref(Mid, x, y, 0, 0.0))
# lights: over where the clone stands (+y of the centre), and behind the player
refs.append(ref(LightShadow, 0.0, 150.0, 200.0))
refs.append(ref(LightShadow, 0.0, -300.0, 200.0))
# cell
xcll = b''.join([
    bytes((120, 118, 115, 0)),      # ambient
    bytes((140, 135, 125, 0)),      # directional
    bytes((0, 0, 0, 0)),            # fog near colour
    struct.pack('<ff', 0.0, 20000.0),  # fog near, far
    struct.pack('<ii', 45, 45),     # directional rotation XY, Z
    struct.pack('<f', 1.0),         # directional fade
    struct.pack('<f', 0.0),         # fog clip distance
    struct.pack('<f', 1.0),         # fog power
    bytes((120, 118, 115, 0)) * 6,  # directional ambient x+ x- y+ y- z+ z-
    bytes((0, 0, 0, 0)),            # specular
    struct.pack('<f', 1.0),         # fresnel power
    bytes((0, 0, 0, 0)),            # fog far colour
    struct.pack('<f', 1.0),         # fog max
    struct.pack('<ff', 4000.0, 8000.0),  # light fade begin / end
    struct.pack('<I', 0),           # inherit nothing from a lighting template
])
assert len(xcll) == 92
cellBody = (sub(b'EDID', z('AnimationWatchViewingRoom')) + sub(b'FULL', z('AnimationWatch viewing room')) +
            sub(b'DATA', struct.pack('<H', 0x0001)) + sub(b'XCLL', xcll) + sub(b'LTMP', struct.pack('<I', 0)))
def record(t, fid, body, flags=0):
    return t + struct.pack('<IIIIHH', len(body), flags, fid, 0, 44, 0) + body
def group(label, gtype, content):
    return b'GRUP' + struct.pack('<I', 24 + len(content)) + label + struct.pack('<iHHI', gtype, 0, 0, 0) + content
cellId = own
obj = cellId & 0xFFFFFF
refsBytes = b''.join(record(t, f, b) for t, f, b in refs)
temp = group(struct.pack('<I', cellId), 9, refsBytes)
children = group(struct.pack('<I', cellId), 6, temp)
cellRec = record(b'CELL', cellId, cellBody)
subblock = group(struct.pack('<i', (obj // 10) % 10), 3, cellRec + children)
block = group(struct.pack('<i', obj % 10), 2, subblock)
top = group(b'CELL', 0, block)
nrec = 1 + len(refs) + 4
tes4body = (sub(b'HEDR', struct.pack('<fII', 1.71, nrec, nid[0])) + sub(b'CNAM', z('tonycubed2')) +
            sub(b'SNAM', z('AnimationWatch: private viewing room for the animation clone')) +
            sub(b'MAST', z('Skyrim.esm')) + sub(b'DATA', struct.pack('<Q', 0)))
tes4 = record(b'TES4', 0, tes4body, flags=0x200)
open(sys.argv[1], 'wb').write(tes4 + top)
print('refs', len(refs), 'next id', hex(nid[0]))
