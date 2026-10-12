"""QOI encoder (https://qoiformat.org/qoi-specification.pdf) for the store's
icons and screenshots (docs/repository.md §5.3). RGBA in, bytes out."""
import struct


def encode(width, height, rgba, channels=4):
    """rgba: bytes of width*height pixels, 4 bytes each."""
    out = bytearray(b"qoif" + struct.pack(">IIBB", width, height, channels, 0))
    index = [(0, 0, 0, 0)] * 64
    prev = (0, 0, 0, 255)
    run = 0
    n = width * height
    for i in range(n):
        px = tuple(rgba[i * 4:i * 4 + 4])
        if px == prev:
            run += 1
            if run == 62 or i == n - 1:
                out.append(0xC0 | (run - 1))
                run = 0
            continue
        if run:
            out.append(0xC0 | (run - 1))
            run = 0
        r, g, b, a = px
        h = (r * 3 + g * 5 + b * 7 + a * 11) % 64
        if index[h] == px:
            out.append(h)
        else:
            index[h] = px
            if a == prev[3]:
                dr = (r - prev[0] + 128) % 256 - 128
                dg = (g - prev[1] + 128) % 256 - 128
                db = (b - prev[2] + 128) % 256 - 128
                dr_dg, db_dg = dr - dg, db - dg
                if -2 <= dr <= 1 and -2 <= dg <= 1 and -2 <= db <= 1:
                    out.append(0x40 | (dr + 2) << 4 | (dg + 2) << 2 | (db + 2))
                elif -32 <= dg <= 31 and -8 <= dr_dg <= 7 and -8 <= db_dg <= 7:
                    out.append(0x80 | (dg + 32))
                    out.append((dr_dg + 8) << 4 | (db_dg + 8))
                else:
                    out += bytes((0xFE, r, g, b))
            else:
                out += bytes((0xFF, r, g, b, a))
        prev = px
    return bytes(out + b"\0" * 7 + b"\1")


def decode(data):
    """(width, height, rgba bytes): for tests, and the reference the C decoder is checked against."""
    if data[:4] != b"qoif":
        raise ValueError("not QOI")
    width, height = struct.unpack(">II", data[4:12])
    index = [(0, 0, 0, 0)] * 64
    px = (0, 0, 0, 255)
    out = bytearray()
    p = 14
    while len(out) < width * height * 4:
        b = data[p]
        p += 1
        if b == 0xFE:
            px = (data[p], data[p + 1], data[p + 2], px[3])
            p += 3
        elif b == 0xFF:
            px = tuple(data[p:p + 4])
            p += 4
        elif b >> 6 == 0:
            px = index[b]
        elif b >> 6 == 1:
            px = ((px[0] + (b >> 4 & 3) - 2) % 256, (px[1] + (b >> 2 & 3) - 2) % 256,
                  (px[2] + (b & 3) - 2) % 256, px[3])
        elif b >> 6 == 2:
            b2 = data[p]
            p += 1
            dg = (b & 0x3F) - 32
            px = ((px[0] + dg + (b2 >> 4) - 8) % 256, (px[1] + dg) % 256,
                  (px[2] + dg + (b2 & 15) - 8) % 256, px[3])
        else:
            for _ in range((b & 0x3F) + 1):
                out += bytes(px)
            index[(px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64] = px
            continue
        index[(px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64] = px
        out += bytes(px)
    return width, height, bytes(out[:width * height * 4])
