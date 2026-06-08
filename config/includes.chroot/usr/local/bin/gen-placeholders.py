import struct, zlib

def make_png(width, height, r, g, b, filename):
    def chunk(chunk_type, data):
        c = chunk_type + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)

    header = b'\x89PNG\r\n\x1a\n'
    ihdr = chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))

    raw = b''
    for y in range(height):
        raw += b'\x00'
        for x in range(width):
            raw += struct.pack('BBB', r, g, b)

    idat = chunk(b'IDAT', zlib.compress(raw))
    iend = chunk(b'IEND', b'')

    with open(filename, 'wb') as f:
        f.write(header + ihdr + idat + iend)

import os, sys
out = sys.argv[1] if len(sys.argv) > 1 else '/tmp'
make_png(1920, 1080, 33, 33, 45, os.path.join(out, 'background.png'))
make_png(800, 600, 20, 20, 30, os.path.join(out, 'splash.png'))
make_png(96, 96, 40, 40, 60, os.path.join(out, 'splash_no_bg.png'))
make_png(1920, 1080, 30, 30, 40, os.path.join(out, 'hell_prospect.png'))
print("Created placeholder images in", out)
