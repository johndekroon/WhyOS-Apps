# SPDX-License-Identifier: MIT
"""The parts of CPython's library that meshcore_py imports and MicroPython
lacks, as far as meshcore_py uses them. meshcore/__init__.py puts this one
module in sys.modules under each name (enum, typing, dataclasses, inspect,
json, traceback, struct, secrets, Crypto.*, serial_asyncio_fast) while the
library is imported; only secrets stays, for the import inside a function.
"""
import hashlib
import io
import os
import sys

# --- enum ---------------------------------------------------------------
# MicroPython has no metaclasses, so a class body cannot turn its own
# attributes into members: enum_members() does that afterwards, for each
# Enum subclass, before any other module uses them.


class Enum:
    def __new__(cls, value):
        # EventType("contact_message"): the member with that value
        for m in cls._members:
            if m.value == value:
                return m
        raise ValueError("%r is not a valid %s" % (value, cls.__name__))

    def __repr__(self):
        return "<%s.%s: %r>" % (type(self).__name__, self.name, self.value)

    def __str__(self):
        return "%s.%s" % (type(self).__name__, self.name)


def enum_members(cls):
    cls._members = []
    for name in dir(cls):
        if name.startswith("_"):
            continue
        v = getattr(cls, name)
        if callable(v) or isinstance(v, cls):
            continue
        # a second name for a value is an alias of the first, as in CPython
        m = None
        for o in cls._members:
            if o.value == v:
                m = o
        if m is None:
            m = object.__new__(cls)
            m.name = name
            m.value = v
            cls._members.append(m)
        setattr(cls, name, m)


# --- typing: annotations are not evaluated, but aliases at module level are


class _Type:
    def __getitem__(self, k):
        return self

    def __call__(self, *a, **k):
        return self


Any = Awaitable = Callable = Coroutine = Dict = List = Optional = Set = Tuple = Union = _Type()
TYPE_CHECKING = False


class Protocol:
    pass


# --- dataclasses: the library's dataclasses define their own __init__


def dataclass(cls=None, **kw):
    return cls if cls is not None else (lambda c: c)


def field(default=None, default_factory=None, **kw):
    return default_factory() if default_factory is not None else default


# --- inspect: MicroPython makes no difference between a generator and a
# coroutine, nor can it see the function inside a closure or bound method

_g = lambda: (yield)  # noqa: E731


def iscoroutine(obj):
    return isinstance(obj, type(_g()))


def iscoroutinefunction(obj):
    return isinstance(obj, type(_g))


# --- json: MicroPython's, whose dumps() takes no default= (the library
# turns sensor data into JSON with one)

import json as _json


def _plain(o, default):
    if isinstance(o, dict):
        return dict((k, _plain(v, default)) for k, v in o.items())
    if isinstance(o, (list, tuple)):
        return [_plain(v, default) for v in o]
    if o is None or isinstance(o, (bool, int, float, str)) or default is None:
        return o
    return _plain(default(o), default)


def dumps(obj, default=None, **kw):
    return _json.dumps(_plain(obj, default))


def dump(obj, fp, default=None, **kw):
    fp.write(dumps(obj, default))


loads = _json.loads
load = _json.load

# --- traceback


def format_exc():
    e = sys.exc_info()[1]
    if e is None:
        return "NoneType: None\n"
    s = io.StringIO()
    sys.print_exception(e, s)
    return s.getvalue()


# --- struct: MicroPython's, which takes no spaces in a format ('<H I H B')

import struct as _struct


def calcsize(fmt):
    return _struct.calcsize(fmt.replace(" ", ""))


def pack(fmt, *v):
    return _struct.pack(fmt.replace(" ", ""), *v)


def pack_into(fmt, buf, offset, *v):
    return _struct.pack_into(fmt.replace(" ", ""), buf, offset, *v)


def unpack(fmt, data):
    return _struct.unpack(fmt.replace(" ", ""), data)


def unpack_from(fmt, data, offset=0):
    return _struct.unpack_from(fmt.replace(" ", ""), data, offset)


error = ValueError

# --- secrets


def token_bytes(n=32):
    return os.urandom(n)


def token_hex(n=32):
    return os.urandom(n).hex()


# --- Crypto (pycryptodome): AES-ECB, SHA-256 and HMAC-SHA256, with
# MicroPython's cryptolib and hashlib


class _AesEcb:
    def __init__(self, key):
        self._key = bytes(key)

    # cryptolib's aes objects encrypt or decrypt, not both: one per call
    def encrypt(self, data):
        import cryptolib

        return cryptolib.aes(self._key, 1).encrypt(data)

    def decrypt(self, data):
        import cryptolib

        return cryptolib.aes(self._key, 1).decrypt(data)


class AES:
    MODE_ECB = 1
    block_size = 16

    @staticmethod
    def new(key, mode, *a, **k):
        if mode != AES.MODE_ECB:
            raise ValueError("only AES.MODE_ECB")
        return _AesEcb(key)


class _Sha256:
    digest_size = 32
    block_size = 64

    # hashlib's objects cannot update after digest(): keep the data
    def __init__(self, data=None):
        self._data = bytearray(data or b"")

    def update(self, data):
        self._data += data
        return self

    def digest(self):
        return hashlib.sha256(bytes(self._data)).digest()

    def hexdigest(self):
        return self.digest().hex()


class SHA256:
    digest_size = 32
    block_size = 64

    @staticmethod
    def new(data=None):
        return _Sha256(data)


class _Hmac:
    def __init__(self, key, msg, digestmod):
        self._mod = digestmod
        key = bytes(key)
        if len(key) > digestmod.block_size:
            key = digestmod.new(key).digest()
        key = key + bytes(digestmod.block_size - len(key))
        self._outer = bytes(b ^ 0x5C for b in key)
        self._inner = digestmod.new(bytes(b ^ 0x36 for b in key))
        if msg:
            self._inner.update(msg)

    def update(self, msg):
        self._inner.update(msg)
        return self

    def digest(self):
        return self._mod.new(self._outer + self._inner.digest()).digest()

    def hexdigest(self):
        return self.digest().hex()


class HMAC:
    @staticmethod
    def new(key, msg=b"", digestmod=None):
        return _Hmac(key, msg, digestmod or SHA256)


# --- serial_asyncio_fast: SerialConnection needs pyserial


async def create_serial_connection(*a, **k):
    raise OSError("no serial companion here: use MeshCore.create_unix()")
