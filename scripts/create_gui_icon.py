"""Reproduce the project's geometric app icon using only the Python standard library."""
from pathlib import Path
import struct
import zlib

def inside(x, y, polygon):
    crosses = False
    for i, (ax, ay) in enumerate(polygon):
        bx, by = polygon[i - 1]
        if (ay > y) != (by > y) and x < (bx - ax) * (y - ay) / (by - ay) + ax:
            crosses = not crosses
    return crosses

shapes = [([(64, 23), (102, 44), (64, 65), (26, 44)], (109, 224, 205, 255)),
          ([(26, 50), (61, 70), (61, 109), (26, 88)], (47, 168, 182, 255)),
          ([(67, 70), (102, 50), (102, 88), (67, 109)], (188, 241, 234, 255))]

def pixel(x, y):
    dx, dy = max(22 - x, 0, x - 106), max(22 - y, 0, y - 106)
    if dx * dx + dy * dy > 22 * 22:
        return (0, 0, 0, 0)
    color = (18, 52, 63, 255)
    for polygon, fill in shapes:
        if inside(x, y, polygon):
            color = fill
    return color

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

images = []
for size in (16, 32, 48, 128, 256):
    raw = bytearray()
    for y in range(size):
        raw.append(0)
        for x in range(size):
            colors = [pixel((x + (i + .5) / 2) * 128 / size, (y + (j + .5) / 2) * 128 / size) for j in range(2) for i in range(2)]
            raw.extend(sum(c[k] for c in colors) // 4 for k in range(4))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b'')
    images.append((size, png))
offset = 6 + 16 * len(images)
ico = struct.pack('<HHH', 0, 1, len(images))
for size, png in images:
    ico += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(png), offset)
    offset += len(png)
ico += b''.join(png for _, png in images)
target = Path(__file__).resolve().parents[1] / 'apps/GeoModelBridge.Gui/Assets/AppIcon.ico'
target.parent.mkdir(parents=True, exist_ok=True)
target.write_bytes(ico)
print(target)
