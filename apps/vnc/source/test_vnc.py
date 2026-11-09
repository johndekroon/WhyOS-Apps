#!/usr/bin/env python3
"""vncd (sdapps/vnc/vncd.c) on the computer, with an RFB viewer written here.

    python3 sdapps/vnc/test_vnc.py [--rv32]

In the build VM: vncd is built with the computer's compiler (DES from the
mbedtls sources build.sh fetched) and serves a 720x720 RGB565 file in place
of /dev/fb0, with a file in place of the keypad's event device. --rv32
builds it with the badge's compiler and C library instead and runs it under
qemu-riscv32. Standard library only: the viewer's DES is below.
"""
import array
import os
import random
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
MBEDTLS = os.path.expanduser("~/sdapps/src/mbedtls-3.6.6")
PREFIX = os.path.expanduser("~/sdapps/prefix")
CC_RV32 = os.path.expanduser("~/br-badge/host/bin/riscv32-buildroot-linux-uclibc-gcc")
W = H = 720
ALPHABET = "abcdefghjkmnpqrstuvwxyz23456789"
EV_SYN, EV_KEY = 0, 1
KEY_A, KEY_B, KEY_LEFTSHIFT, KEY_DELETE, KEY_LEFTMETA, KEY_F11 = 30, 48, 42, 111, 125, 87

# DES (FIPS 46-3), enough for VNC authentication

IP = [58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6,
      64, 56, 48, 40, 32, 24, 16, 8, 57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
      61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7]
FP = [IP.index(i) + 1 for i in range(1, 65)]
E = [32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
     16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1]
P = [16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
     2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25]
PC1 = [57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27, 19, 11, 3,
       60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37,
       29, 21, 13, 5, 28, 20, 12, 4]
PC2 = [14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
       41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32]
SHIFTS = [1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1]
SBOX = [
    [14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
     4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13],
    [15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
     0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9],
    [10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
     13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12],
    [7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
     10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14],
    [2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
     4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3],
    [12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
     9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13],
    [4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
     1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12],
    [13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
     7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11],
]


def permute(v, table, width):
    out = 0
    for t in table:
        out = out << 1 | (v >> (width - t)) & 1
    return out


def des_encrypt(key, block):
    k = permute(int.from_bytes(key, "big"), PC1, 64)
    c, d = k >> 28, k & 0xfffffff
    subkeys = []
    for s in SHIFTS:
        c = (c << s | c >> (28 - s)) & 0xfffffff
        d = (d << s | d >> (28 - s)) & 0xfffffff
        subkeys.append(permute(c << 28 | d, PC2, 56))
    v = permute(int.from_bytes(block, "big"), IP, 64)
    left, right = v >> 32, v & 0xffffffff
    for sk in subkeys:
        x = permute(right, E, 32) ^ sk
        f = 0
        for i in range(8):
            six = (x >> (42 - 6 * i)) & 63
            f = f << 4 | SBOX[i][(six & 32) | (six & 1) << 4 | (six >> 1) & 15]
        left, right = right, left ^ permute(f, P, 32)
    return permute(right << 32 | left, FP, 64).to_bytes(8, "big")


def vnc_response(password, challenge):
    key = bytes(int(f"{b:08b}"[::-1], 2) for b in password.encode()[:8].ljust(8, b"\0"))
    return des_encrypt(key, challenge[:8]) + des_encrypt(key, challenge[8:])


# The viewer

class Closed(Exception):
    pass


class Viewer:
    def __init__(self, port, rcvbuf=None):
        self.s = socket.socket()
        if rcvbuf:
            self.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.s.settimeout(10)
        self.s.connect(("127.0.0.1", port))
        self.bpp, self.big = 16, False
        self.fmt, self.depth = DEFAULT, 16
        self.zd = zlib.decompressobj()

    def recv(self, n):
        b = b""
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise Closed(f"closed after {len(b)} of {n} bytes")
            b += c
        return b

    def u8(self):
        return self.recv(1)[0]

    def u16(self):
        return struct.unpack(">H", self.recv(2))[0]

    def u32(self):
        return struct.unpack(">I", self.recv(4))[0]

    def reason(self):
        return self.recv(self.u32()).decode()

    def closed(self, wait=5):
        """True when the server closes the connection within wait seconds."""
        self.s.settimeout(wait)
        try:
            while self.s.recv(4096):
                pass
            return True
        except (socket.timeout, TimeoutError):
            return False
        except ConnectionResetError:
            return True

    def handshake(self, password, version=b"RFB 003.008\n"):
        """Returns (result, reason): 0 and the ServerInit's fields when it worked."""
        assert self.recv(12) == b"RFB 003.008\n"
        self.s.sendall(version)
        v = int(version[8:11])
        if v >= 7:
            n = self.u8()
            if n == 0:
                return "refused", self.reason()
            types = self.recv(n)
            assert types == b"\x02", types
            self.s.sendall(b"\x02")
        else:
            t = self.u32()
            if t == 0:
                return "refused", self.reason()
            assert t == 2, t
        challenge = self.recv(16)
        self.s.sendall(vnc_response(password, challenge))
        result = self.u32()
        if result:
            return "failed", self.reason() if v == 8 else ""
        self.s.sendall(b"\x01")
        w, h = self.u16(), self.u16()
        fmt = self.recv(16)
        name = self.reason()
        return "ok", (w, h, fmt, name)

    def set_pixel_format(self, bpp, big, rmax, gmax, bmax, rs, gs, bs, true_colour=1, depth=None):
        self.bpp, self.big = bpp, big
        self.fmt = (bpp, big, rmax, gmax, bmax, rs, gs, bs)
        self.depth = depth or (24 if bpp == 32 else bpp)
        self.s.sendall(struct.pack(">B3xBBBBHHHBBB3x", 0, bpp, self.depth, big, true_colour,
                                   rmax, gmax, bmax, rs, gs, bs))

    def set_encodings(self, *enc):
        self.s.sendall(struct.pack(">BxH", 2, len(enc)) + b"".join(struct.pack(">i", e) for e in enc))

    def request(self, incremental, x=0, y=0, w=W, h=H):
        self.s.sendall(struct.pack(">BBHHHH", 3, incremental, x, y, w, h))

    def key(self, down, sym):
        self.s.sendall(struct.pack(">BBxxI", 4, down, sym))

    def update(self):
        """An update: a list of (x, y, w, h, encoding, pixel bytes, hextile kinds)."""
        t = self.u8()
        assert t == 0, f"message {t}"
        self.recv(1)
        rects = []
        for _ in range(self.u16()):
            x, y, w, h, enc = struct.unpack(">HHHHi", self.recv(12))
            if enc == 0:
                rects.append((x, y, w, h, enc, self.recv(w * h * self.bpp // 8), None))
            elif enc == 5:
                pix, kinds = self.hextile(w, h)
                rects.append((x, y, w, h, enc, pix, kinds))
            elif enc == 16:
                data = self.zd.decompress(self.recv(self.u32()))
                pix, kinds = self.zrle(data, w, h)
                rects.append((x, y, w, h, enc, pix, kinds))
            else:
                raise AssertionError(f"encoding {enc}")
        return rects

    def hextile(self, w, h):
        bp = self.bpp // 8
        out = bytearray(w * h * bp)
        bg = fg = None
        kinds = {}
        for ty in range(0, h, 16):
            th = min(16, h - ty)
            for tx in range(0, w, 16):
                tw = min(16, w - tx)
                sub = self.u8()
                kinds[sub] = kinds.get(sub, 0) + 1
                if sub & 1:
                    raw = self.recv(tw * th * bp)
                    for j in range(th):
                        o = ((ty + j) * w + tx) * bp
                        out[o:o + tw * bp] = raw[j * tw * bp:(j + 1) * tw * bp]
                    bg = fg = None
                    continue
                if sub & 2:
                    bg = self.recv(bp)
                assert bg is not None, "background not known"
                tile = [[bg] * tw for _ in range(th)]
                if sub & 4:
                    fg = self.recv(bp)
                if sub & 8:
                    for _ in range(self.u8()):
                        c = self.recv(bp) if sub & 16 else fg
                        xy, wh = self.u8(), self.u8()
                        for j in range(xy & 15, (xy & 15) + (wh & 15) + 1):
                            for i in range(xy >> 4, (xy >> 4) + (wh >> 4) + 1):
                                tile[j][i] = c
                for j in range(th):
                    o = ((ty + j) * w + tx) * bp
                    out[o:o + tw * bp] = b"".join(tile[j])
        return bytes(out), kinds

    def zrle(self, data, w, h):
        """ZRLE's tiles: the pixels, and how many tiles of each subencoding."""
        bp = self.bpp // 8
        bpp, big, rmax, gmax, bmax, rs, gs, bs = self.fmt
        allbits = rmax << rs | gmax << gs | bmax << bs
        cp, pad = bp, None
        if bpp == 32 and allbits <= 0xffffff:
            cp, pad = 3, "high"
        elif bpp == 32 and not allbits & 0xff:
            cp, pad = 3, "low"

        def full(c):
            # the CPIXEL as the whole pixel: the byte left out is zero
            if pad is None:
                return c
            return (c + b"\0") if (pad == "high") != big else (b"\0" + c)

        out = bytearray(w * h * bp)
        kinds = {}
        o = 0

        def cpixel():
            nonlocal o
            c = data[o:o + cp]
            o += cp
            return full(c)

        def run_len():
            nonlocal o
            n = 1
            while data[o] == 255:
                n += 255
                o += 1
            n += data[o]
            o += 1
            return n

        for ty in range(0, h, 64):
            th = min(64, h - ty)
            for tx in range(0, w, 64):
                tw = min(64, w - tx)
                sub = data[o]
                o += 1
                kinds[sub] = kinds.get(sub, 0) + 1
                px = []
                if sub == 0:
                    px = [cpixel() for _ in range(tw * th)]
                elif sub == 1:
                    px = [cpixel()] * (tw * th)
                elif 2 <= sub <= 16:
                    pal = [cpixel() for _ in range(sub)]
                    bits = 1 if sub == 2 else 2 if sub <= 4 else 4
                    for _ in range(th):
                        row_bytes = (tw * bits + 7) // 8
                        acc = int.from_bytes(data[o:o + row_bytes], "big")
                        o += row_bytes
                        total = row_bytes * 8
                        for i in range(tw):
                            px.append(pal[(acc >> (total - bits * (i + 1))) & ((1 << bits) - 1)])
                elif sub == 128:
                    while len(px) < tw * th:
                        c = cpixel()
                        px += [c] * run_len()
                elif sub >= 130:
                    pal = [cpixel() for _ in range(sub - 128)]
                    while len(px) < tw * th:
                        k = data[o]
                        o += 1
                        px += [pal[k & 127]] * (run_len() if k & 128 else 1)
                else:
                    raise AssertionError(f"ZRLE subencoding {sub}")
                assert len(px) == tw * th, f"tile of {len(px)} pixels, not {tw * th}"
                for j in range(th):
                    q = ((ty + j) * w + tx) * bp
                    out[q:q + tw * bp] = b"".join(px[j * tw:(j + 1) * tw])
        assert o == len(data), f"{len(data) - o} bytes left over"
        return bytes(out), kinds

    def close(self):
        self.s.close()


# What the viewer should see

def convert_table(bpp, big, rmax, gmax, bmax, rs, gs, bs):
    order = "big" if big else "little"
    r = [(i * rmax + 15) // 31 << rs for i in range(32)]
    g = [(i * gmax + 31) // 63 << gs for i in range(64)]
    b = [(i * bmax + 15) // 31 << bs for i in range(32)]
    mask = (1 << bpp) - 1
    return [((r[v >> 11] | g[(v >> 5) & 63] | b[v & 31]) & mask).to_bytes(bpp // 8, order) for v in range(65536)]


DEFAULT = (16, False, 31, 63, 31, 11, 5, 0)


def expected(pixels, table, x, y, w, h):
    return b"".join(table[pixels[(j * W) + i]] for j in range(y, y + h) for i in range(x, x + w))


def picture(seed):
    """Solid blocks, gradients and noise: both kinds of hextile tile."""
    rnd = random.Random(seed)
    px = array.array("H", [0] * (W * H))
    for y in range(H):
        for x in range(W):
            if y < 200:
                v = 0x07e0 if (x // 64 + y // 64) % 2 else 0xf800
            elif y < 400:
                v = ((x * 31 // W) << 11) | ((y * 63 // H) << 5) | (x * y) % 32
            elif x < 360:
                v = rnd.getrandbits(16)
            else:
                v = 0x001f
            px[y * W + x] = v
    return px


class Server:
    def __init__(self, exe, tmp, port, prefix=(), event_size=24, datadir=None, env=None):
        self.fb = os.path.join(tmp, "fb")
        self.input = os.path.join(tmp, "input")
        self.datadir = datadir or os.path.join(tmp, "data")
        self.port = port
        self.event_size = event_size
        self.cmd = list(prefix) + [exe]
        open(self.input, "wb").close()
        self.log = open(os.path.join(tmp, f"vncd-{port}.log"), "w+")
        self.p = subprocess.Popen(list(prefix) + [exe, "-p", str(port), "-f", self.fb, "-i", self.input,
                                                  "-d", self.datadir], stderr=self.log,
                                  env=dict(os.environ, **(env or {})))
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), 0.2).close()
                break
            except OSError:
                time.sleep(0.05)
        # the probe connection above is a viewer that left in the handshake
        time.sleep(0.1)

    def password(self):
        with open(os.path.join(self.datadir, "password")) as f:
            return f.read().strip()

    def events(self):
        with open(self.input, "rb") as f:
            data = f.read()
        n = self.event_size
        out = []
        for i in range(0, len(data) - n + 1, n):
            t, c, v = struct.unpack("<HHi", data[i + n - 8:i + n])
            out.append((t, c, v))
        return out

    def alive(self):
        return self.p.poll() is None

    def stop(self):
        self.p.terminate()
        self.p.wait(10)
        self.log.seek(0)
        return self.log.read()


# The tests

RESULTS = []


def check(name, cond, detail=""):
    RESULTS.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else f": {detail}"), flush=True)


def connect(srv, pw, version=b"RFB 003.008\n"):
    v = Viewer(srv.port)
    r = v.handshake(pw, version)
    if r[0] != "ok":
        raise AssertionError(f"handshake {r}")
    return v


def sync(v):
    """A round trip: everything sent before has been handled."""
    v.request(0, 0, 0, 1, 1)
    v.update()


def key_events(srv):
    return [e for e in srv.events()]


def run(srv, px):
    pw = srv.password()
    st = os.stat(os.path.join(srv.datadir, "password"))
    check("password file made, mode 0600, 8 characters from the alphabet",
          stat.S_IMODE(st.st_mode) == 0o600 and len(pw) == 8 and all(c in ALPHABET for c in pw), (pw, oct(st.st_mode)))
    r = subprocess.run(srv.cmd + ["-n", "-d", srv.datadir], capture_output=True, timeout=30)
    new = srv.password()
    check("vncd -n: a new password, and the running vncd takes it",
          r.returncode == 0 and new != pw and len(new) == 8 and Viewer(srv.port).handshake(new)[0] == "ok",
          (r.returncode, pw, new))
    pw = new

    # versions
    for ver in (b"RFB 003.008\n", b"RFB 003.007\n", b"RFB 003.003\n", b"RFB 003.889\n"):
        v = Viewer(srv.port)
        r = v.handshake(pw, ver)
        check(f"handshake {ver[4:11].decode()}: ServerInit 720x720 RGB565",
              r[0] == "ok" and r[1][:2] == (W, H) and r[1][2][:10] == bytes([16, 16, 0, 1, 0, 31, 0, 63, 0, 31])
              and r[1][2][10:13] == bytes([11, 5, 0]), r)
        v.close()

    # wrong password (fewer than five: that is the last test)
    for ver in (b"RFB 003.008\n", b"RFB 003.003\n"):
        v = Viewer(srv.port)
        r = v.handshake("wrong", ver)
        check(f"wrong password refused ({ver[4:11].decode()})",
              r[0] == "failed" and (r[1] == "Wrong password" if ver[10] == ord("8") else True) and v.closed(), r)
        v.close()

    # the full picture, in several formats, raw
    table16 = convert_table(*DEFAULT)
    v = connect(srv, pw)
    v.set_encodings(0)
    v.request(0)
    rects = v.update()
    check("full update, 16 bpp default: one raw rectangle equal to the file",
          len(rects) == 1 and rects[0][:5] == (0, 0, W, H, 0) and rects[0][5] == px.tobytes(), rects[0][:5] if rects else rects)
    formats = [
        ("32 bpp little-endian (as macOS asks)", (32, False, 255, 255, 255, 16, 8, 0)),
        ("32 bpp big-endian, BGR", (32, True, 255, 255, 255, 0, 8, 16)),
        ("16 bpp big-endian RGB565", (16, True, 31, 63, 31, 11, 5, 0)),
        ("16 bpp RGB555", (16, False, 31, 31, 31, 10, 5, 0)),
        ("8 bpp BGR233", (8, False, 7, 7, 3, 0, 3, 6)),
    ]
    for name, fmt in formats:
        v.set_pixel_format(*fmt)
        v.request(0)
        rects = v.update()
        want = expected(px, convert_table(*fmt), 0, 0, W, H)
        check(f"full update, {name}", len(rects) == 1 and rects[0][5] == want)
    # part of the screen, non-incremental
    v.set_pixel_format(*DEFAULT)
    v.request(0, 100, 150, 33, 17)
    rects = v.update()
    check("non-incremental part (100,150 33x17)",
          len(rects) == 1 and rects[0][:4] == (100, 150, 33, 17) and rects[0][5] == expected(px, table16, 100, 150, 33, 17),
          rects[0][:4] if rects else rects)

    # incremental: nothing changed, then a tile
    v.request(1)
    v.s.settimeout(0.5)
    try:
        v.s.recv(1)
        quiet = False
    except (socket.timeout, TimeoutError):
        quiet = True
    v.s.settimeout(10)
    check("incremental request without changes: no answer yet", quiet)

    def change(points):
        with open(srv.fb, "r+b") as f:
            for x, y, val in points:
                px[y * W + x] = val
                f.seek((y * W + x) * 2)
                f.write(struct.pack("<H", val))

    t0 = time.time()
    change([(50, 85, 0x1234)])
    rects = v.update()
    check("incremental update after a pixel changed: just its tile (48,80 16x16)",
          [r[:4] for r in rects] == [(48, 80, 16, 16)] and rects[0][5] == expected(px, table16, 48, 80, 16, 16),
          [r[:4] for r in rects])
    check("... within 0.5 s", time.time() - t0 < 0.5, time.time() - t0)
    change([(20, 300, 0xffff), (36, 295, 0x0001), (700, 719, 0xabcd)])
    v.request(1)
    rects = v.update()
    check("incremental update: adjacent tiles merged, another row apart",
          sorted(r[:4] for r in rects) == [(16, 288, 32, 16), (688, 704, 16, 16)], [r[:4] for r in rects])
    check("... pixels right", all(r[5] == expected(px, table16, *r[:4]) for r in rects))

    # hextile equals raw
    for name, fmt in (("16 bpp", DEFAULT), ("32 bpp", formats[0][1])):
        v.set_pixel_format(*fmt)
        v.set_encodings(5, 0)
        v.request(0)
        hx = v.update()
        v.set_encodings(0)
        v.request(0)
        raw = v.update()
        kinds = hx[0][6] if hx else {}
        check(f"Hextile ({name}) decodes to the same pixels as Raw",
              len(hx) == 1 and hx[0][4] == 5 and hx[0][5] == raw[0][5] == expected(px, convert_table(*fmt), 0, 0, W, H))
        check(f"... with solid tiles (new and repeated background) and raw ones: {kinds}",
              kinds.get(1) and kinds.get(2) and kinds.get(0))
    v.set_pixel_format(*DEFAULT)
    v.set_encodings(5)
    v.request(0, 5, 7, 50, 21)
    hx = v.update()
    check("Hextile of an odd rectangle (5,7 50x21)",
          len(hx) == 1 and hx[0][5] == expected(px, table16, 5, 7, 50, 21))

    # ZRLE: in strips of 64 rows, each the same pixels as Raw; one zlib stream for all
    zformats = [("16 bpp", DEFAULT), ("32 bpp little-endian, 3-byte CPIXEL", formats[0][1]),
                ("32 bpp big-endian BGR, 3-byte CPIXEL", formats[1][1]),
                ("32 bpp big-endian, colours in the top bytes", (32, True, 255, 255, 255, 24, 16, 8)),
                ("8 bpp", formats[4][1])]
    zformats.append(("32 bpp of depth 32 (as macOS asks): still a 3-byte CPIXEL", formats[0][1], 32))
    for name, fmt, *depth in zformats:
        v.set_pixel_format(*fmt, depth=depth[0] if depth else None)
        v.set_encodings(16, 5, 0)
        v.request(0)
        z = v.update()
        want = expected(px, convert_table(*fmt), 0, 0, W, H)
        got = b"".join(r[5] for r in sorted(z, key=lambda r: r[1]))
        kinds = {}
        for r in z:
            for k, n in r[6].items():
                kinds[k] = kinds.get(k, 0) + n
        check(f"ZRLE ({name}): 12 strips of 64 rows, the same pixels as Raw",
              len(z) == 12 and all(r[4] == 16 and r[2] == W for r in z) and got == want,
              [r[:5] for r in z][:3])
        check(f"... solid tiles and raw or run tiles: {kinds}", kinds.get(1) and (kinds.get(0) or kinds.get(128)))
    v.set_pixel_format(*DEFAULT)
    v.request(0, 5, 7, 50, 21)
    z = v.update()
    check("ZRLE of an odd rectangle (5,7 50x21)",
          len(z) == 1 and z[0][5] == expected(px, table16, 5, 7, 50, 21))
    # a palette: a pixel of another colour in a solid tile, then incremental
    v.request(1)
    with open(srv.fb, "r+b") as f:
        px[10 * W + 10] = 0xffff
        f.seek((10 * W + 10) * 2)
        f.write(struct.pack("<H", 0xffff))
    z = v.update()
    sub = [k for r in z for k in r[6]]
    check(f"ZRLE incremental: the changed tile, as a palette of two ({sub})",
          [r[:4] for r in z] == [(0, 0, 16, 16)] and z[0][5] == expected(px, table16, 0, 0, 16, 16)
          and sub and (2 <= sub[0] <= 16 or sub[0] >= 130), [r[:4] for r in z])
    v.set_encodings(0)

    # keys
    open(srv.input, "wb").close()
    v.close()
    time.sleep(0.2)
    v = connect(srv, pw)
    v.key(1, ord("A"))
    v.key(0, ord("A"))
    sync(v)
    ev = key_events(srv)
    check("KeyEvent 'A': shift and a down, a and shift up",
          ev == [(EV_KEY, KEY_LEFTSHIFT, 1), (EV_SYN, 0, 0), (EV_KEY, KEY_A, 1), (EV_SYN, 0, 0),
                 (EV_KEY, KEY_A, 0), (EV_SYN, 0, 0), (EV_KEY, KEY_LEFTSHIFT, 0), (EV_SYN, 0, 0)], ev)
    open(srv.input, "wb").close()
    v.close()
    time.sleep(0.2)
    v = connect(srv, pw)
    v.key(1, 0xffe1)        # Shift_L held by the viewer
    v.key(1, ord("A"))
    v.key(0, ord("A"))
    v.key(0, 0xffe1)
    v.key(1, 0xffeb)        # Super_L: the logo key
    v.key(0, 0xffeb)
    v.key(1, 0xffff)        # Delete
    v.key(1, 0xffff)        # the viewer's repeat
    v.key(0, 0xffff)
    v.key(1, 0x1234567)     # no such key
    v.key(0, 0x1234567)
    v.key(1, 0xffc8)        # F11
    v.key(0, 0xffc8)
    sync(v)
    ev = [e for e in key_events(srv) if e[0] == EV_KEY]
    check("Shift held + 'A', Super_L, Delete (repeat ignored), unknown keysym, F11",
          ev == [(EV_KEY, KEY_LEFTSHIFT, 1), (EV_KEY, KEY_A, 1), (EV_KEY, KEY_A, 0), (EV_KEY, KEY_LEFTSHIFT, 0),
                 (EV_KEY, KEY_LEFTMETA, 1), (EV_KEY, KEY_LEFTMETA, 0), (EV_KEY, KEY_DELETE, 1),
                 (EV_KEY, KEY_DELETE, 0), (EV_KEY, KEY_F11, 1), (EV_KEY, KEY_F11, 0)], ev)
    open(srv.input, "wb").close()
    v.close()
    time.sleep(0.2)
    v = connect(srv, pw)
    v.key(1, ord("b"))
    v.key(1, ord("?"))
    sync(v)
    v.close()
    time.sleep(0.5)
    ev = [e for e in key_events(srv) if e[0] == EV_KEY]
    check("keys still held when the viewer goes are released",
          ev[:3] == [(EV_KEY, KEY_B, 1), (EV_KEY, KEY_LEFTSHIFT, 1), (EV_KEY, 53, 1)]
          and sorted(ev[3:]) == sorted([(EV_KEY, KEY_B, 0), (EV_KEY, 53, 0), (EV_KEY, KEY_LEFTSHIFT, 0)]), ev)

    # pointer and clipboard ignored
    v = connect(srv, pw)
    v.s.sendall(struct.pack(">BBHH", 5, 1, 10, 10))
    text = b"x" * 20000
    v.s.sendall(struct.pack(">BxxxI", 6, len(text)) + text)
    v.request(0, 0, 0, 16, 16)
    rects = v.update()
    check("PointerEvent and 20 kB of ClientCutText are skipped",
          len(rects) == 1 and rects[0][5] == expected(px, table16, 0, 0, 16, 16))

    # a second viewer
    for ver in (b"RFB 003.008\n", b"RFB 003.003\n"):
        v2 = Viewer(srv.port)
        r = v2.handshake(pw, ver)
        check(f"second viewer ({ver[4:11].decode()}) refused: \"Another viewer is connected\"",
              r == ("refused", "Another viewer is connected"), r)
        v2.close()
    sync(v)
    check("... the first one still works", True)

    # malformed input
    v.s.sendall(b"\xee" + os.urandom(30))
    check("unknown message type closes the connection", v.closed())
    v.close()
    v = connect(srv, pw)
    v.set_pixel_format(24, 0, 255, 255, 255, 16, 8, 0)
    check("SetPixelFormat 24 bpp closes the connection", v.closed())
    v.close()
    v = connect(srv, pw)
    v.set_pixel_format(8, 0, 7, 7, 3, 0, 3, 6, true_colour=0)
    check("SetPixelFormat with a colour map closes the connection", v.closed())
    v.close()
    v = connect(srv, pw)
    v.set_pixel_format(16, 0, 31, 63, 31, 16, 5, 0)
    check("SetPixelFormat with a shift past the pixel closes the connection", v.closed())
    v.close()
    v = connect(srv, pw)
    v.s.sendall(struct.pack(">BxH", 2, 5000))
    check("SetEncodings with 5000 encodings closes the connection", v.closed())
    v.close()
    for junk in (b"GET / HTTP/1.1\r\n\r\n", b"RFB 004.000\n", b"RFB 00x.008\n", b"\0" * 12):
        v = Viewer(srv.port)
        v.recv(12)
        v.s.sendall(junk)
        later = junk == b"RFB 004.000\n"
        ok = v.u8() == 1 if later else v.closed()
        v.close()
        check(f"version {junk[:12]!r}: {'taken as 3.8' if later else 'closed'}", ok)
    rnd = random.Random(7)
    for i in range(40):
        v = connect(srv, pw)
        v.s.sendall(bytes(rnd.getrandbits(8) for _ in range(rnd.randint(1, 300))))
        v.s.close()
    v = Viewer(srv.port)
    v.recv(12)
    v.s.sendall(b"RFB 003.008\n")
    v.close()                       # gone in the middle of the handshake
    time.sleep(0.3)
    v = connect(srv, pw)
    sync(v)
    check("40 viewers sending random bytes after the handshake, one leaving in it: server still serves",
          srv.alive())
    v.close()

    # a viewer that stops reading is dropped
    time.sleep(0.2)
    v = Viewer(srv.port, rcvbuf=4096)
    assert v.handshake(pw)[0] == "ok"
    v.set_pixel_format(*formats[0][1])
    v.s.setblocking(False)
    t0 = time.time()
    while time.time() - t0 < 2:
        try:
            v.request(0)
        except BlockingIOError:
            pass
        time.sleep(0.05)
    t0 = time.time()
    while time.time() - t0 < 20:
        v2 = Viewer(srv.port)
        v2.s.settimeout(20)
        r = v2.handshake(pw)
        v2.close()
        if r[0] == "ok":
            break
        time.sleep(1)
    check(f"a viewer that stops reading is dropped, the next one comes in ({time.time() - t0:.1f} s)", r[0] == "ok", r)
    v.close()

    # five wrong passwords: no viewers for a minute
    time.sleep(0.2)
    for i in range(5):
        v = Viewer(srv.port)
        r = v.handshake("nope")
        v.close()
    v = Viewer(srv.port)
    r = v.handshake(pw)
    check("after 5 wrong passwords even the right one is refused",
          r[0] == "refused" and "wrong passwords" in r[1], r)
    v.close()
    check("server still running", srv.alive())


# The keypad's keys (the badge's device tree), for EVIOCGBIT
KEYPAD = [1, 59, 60, 61, 62, 63, 64, 14, 11, 12, 41, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 16, 17, 18, 19, 20, 21, 22,
          23, 24, 0x1d0, 30, 31, 32, 33, 34, 35, 36, 37, 38, 42, 44, 45, 46, 47, 48, 49, 50, 51, 52, 105, 108,
          106, 53, 103, 54, 39, 40, 28, 13, 29, 125, 56, 43, 57, 100, 25, 26, 27]
FAKE_KEYPAD = r"""
#define _GNU_SOURCE
#include <linux/input.h>
#include <stdarg.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
static const int keys[] = { %s };
int ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	unsigned char *p;
	unsigned i;

	va_start(ap, req);
	p = va_arg(ap, unsigned char *);
	va_end(ap);
	if (req != EVIOCGBIT(EV_KEY, KEY_MAX / 8 + 1))
		return syscall(SYS_ioctl, fd, req, p);
	memset(p, 0, KEY_MAX / 8 + 1);
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		p[keys[i] / 8] |= 1 << keys[i] %% 8;
	return KEY_MAX / 8 + 1;
}
"""


def keypad_layer(tmp):
    """Keys the keypad lacks, with its key set from EVIOCGBIT (a preloaded ioctl)."""
    d = os.path.join(tmp, "keypad")
    os.mkdir(d)
    with open(os.path.join(d, "fake.c"), "w") as f:
        f.write(FAKE_KEYPAD % ", ".join(map(str, KEYPAD)))
    so = os.path.join(d, "fake.so")
    subprocess.run(["gcc", "-shared", "-fPIC", "-O2", os.path.join(d, "fake.c"), "-o", so], check=True)
    exe = build(d, False, sanitize=False)
    srv = Server(exe, d, 16900 + os.getpid() % 1000, env={"LD_PRELOAD": so})
    with open(srv.fb, "wb") as f:
        f.write(bytes(W * H * 2))
    try:
        v = connect(srv, srv.password())
        for sym in (0xffff, 0xff50, 0xff57, 0xff55, 0xff56, 0xffc4, 0xffc9, 0xffe4, 0xffec, ord("x")):
            v.key(1, sym)
            v.key(0, sym)
        sync(v)
        ev = [e[1:] for e in srv.events() if e[0] == EV_KEY]
        fn = 0x1d0
        want = []
        for code in (14, 105, 106, 103, 108, 59, 64):   # Delete Home End PgUp PgDn F7 F12
            want += [(fn, 1), (code, 1), (code, 0), (fn, 0)]
        want += [(29, 1), (29, 0), (125, 1), (125, 0), (45, 1), (45, 0)]   # Control_R, Super_R, x
        check("keys the keypad lacks: Fn+Backspace, Fn+arrows, Fn+F1/F6, left Ctrl; Super_R the logo key",
              ev == want, ev)
        v.close()
    finally:
        srv.stop()


def build(tmp, rv32, sanitize=True):
    src = os.path.join(HERE, "vncd.c")
    exe = os.path.join(tmp, "vncd")
    if rv32:
        subprocess.run([CC_RV32, "-Os", "-static", "-Wall", "-I" + PREFIX + "/include", src,
                        "-L" + PREFIX + "/lib", "-lmbedcrypto", "-lz", "-o", exe], check=True)
    else:
        lib = os.path.join(MBEDTLS, "library")
        subprocess.run(["gcc", "-O2", "-g", "-Wall", "-Wextra"] + (["-fsanitize=address,undefined"] if sanitize else []) + [
                        "-I" + os.path.join(MBEDTLS, "include"), src, os.path.join(lib, "des.c"),
                        os.path.join(lib, "platform_util.c"), "-lz", "-o", exe], check=True)
    return exe


def main():
    rv32 = "--rv32" in sys.argv
    assert des_encrypt(bytes.fromhex("133457799BBCDFF1"), bytes.fromhex("0123456789ABCDEF")).hex() == \
        "85e813540f0ab405", "DES"
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp, rv32)
        px = picture(1)
        srv = Server(exe, tmp, 15900 + os.getpid() % 1000, prefix=["qemu-riscv32"] if rv32 else [],
                     event_size=16 if rv32 else 24)
        with open(srv.fb, "wb") as f:
            f.write(px.tobytes())
        try:
            run(srv, px)
        finally:
            log = srv.stop()
        print("--- vncd's log ---\n" + log.strip())
        check("stderr clean of sanitizer reports", "runtime error" not in log and "AddressSanitizer" not in log,
              log[-2000:])
        if not rv32:
            keypad_layer(tmp)
    failed = [n for n, ok in RESULTS if not ok]
    print(f"{len(RESULTS) - len(failed)} of {len(RESULTS)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
