# SPDX-License-Identifier: MIT
"""meshcore_py, the MeshCore companion library, on the badge's MicroPython.

    import asyncio
    from meshcore import MeshCore, EventType

    async def main():
        mc = await MeshCore.create_unix()   # meshcored on this badge
        ...

The library is upstream's, unchanged but for patches/ (sdapps/micropython/
meshcore-py); this file replaces its __init__: it supplies what MicroPython
lacks (_compat, _asyncio, _logging) while the library is imported, and adds
UnixConnection and MeshCore.create_unix() for meshcored's socket.
"""
import asyncio
import sys

from . import _compat, _asyncio, _logging as logging

_asyncio.install()

# The library logs with exc_info=True, which micropython-lib's logging does
# not take: it gets ours, which stays as logging for a bot that imports it
# after this (so logging.basicConfig() shows the library's messages too).
_ALIASES = (
    "enum", "typing", "dataclasses", "inspect", "json", "traceback", "struct", "secrets",
    "Crypto", "Crypto.Cipher", "Crypto.Hash", "serial_asyncio_fast", "logging",
)
_saved = {}
for _n in _ALIASES:
    _saved[_n] = sys.modules.get(_n)
    sys.modules[_n] = logging if _n == "logging" else _compat
try:
    from . import packets, events

    for _m in (packets, events):
        for _v in _m.__dict__.values():
            if isinstance(_v, type) and issubclass(_v, _compat.Enum) and _v is not _compat.Enum:
                _compat.enum_members(_v)

    from .ble_cx import BLEConnection
    from .connection_manager import ConnectionManager
    from .events import EventType
    from .meshcore import MeshCore
    from .packets import BinaryReqType
    from .serial_cx import SerialConnection
    from .tcp_cx import TCPConnection
    from .unix_cx import UnixConnection, SOCKET
    from . import events as _events, serial_cx as _serial_cx, tcp_cx as _tcp_cx
finally:
    for _n in _ALIASES:
        if _saved[_n] is not None:
            sys.modules[_n] = _saved[_n]
        elif _n not in ("secrets", "logging"):  # secrets: imported in a method
            del sys.modules[_n]


# MicroPython's tasks have no add_done_callback(): the task takes itself
# out of the set when it ends
def _spawn_background(self, coro):
    async def run():
        try:
            return await coro
        finally:
            self._background_tasks.discard(task)

    task = asyncio.create_task(run())
    self._background_tasks.add(task)
    return task


for _c in (_events.EventDispatcher, _tcp_cx.TCPConnection, _serial_cx.SerialConnection):
    _c._spawn_background = _spawn_background


# What @dataclass gives Event in CPython: print(event) shows its fields
def _event_repr(self):
    return "Event(type=%r, payload=%r, attributes=%r)" % (self.type, self.payload, self.attributes)


def _event_eq(self, other):
    return type(other) is type(self) and (self.type, self.payload, self.attributes) == (
        other.type, other.payload, other.attributes)


_events.Event.__repr__ = _event_repr
_events.Event.__eq__ = _event_eq


async def _create_unix(
    cls,
    path=SOCKET,
    debug=False,
    only_error=False,
    default_timeout=None,
    auto_reconnect=False,
    max_reconnect_attempts=3,
):
    """Create and connect a MeshCore instance on meshcored's Unix socket"""
    mc = cls(
        UnixConnection(path),
        debug=debug,
        only_error=only_error,
        default_timeout=default_timeout,
        auto_reconnect=auto_reconnect,
        max_reconnect_attempts=max_reconnect_attempts,
    )
    res = await mc.connect()
    if res is None:
        logger.error("No response from meshcore node, disconnecting")
        await mc.disconnect()
        return None
    return mc


MeshCore.create_unix = classmethod(_create_unix)

logger = logging.getLogger(__name__)
logger.addHandler(logging.NullHandler())

__all__ = [
    "BinaryReqType",
    "BLEConnection",
    "ConnectionManager",
    "EventType",
    "MeshCore",
    "SerialConnection",
    "TCPConnection",
    "UnixConnection",
    "logger",
]
