# SPDX-License-Identifier: MIT
"""What meshcore_py uses of CPython's asyncio and MicroPython's lacks:
Future, Queue, wait(), wait_for() on a Future, get_running_loop() with
create_future() and create_connection() for asyncio.Protocol, and
create_unix_connection() for meshcored's socket. install() adds them to
the asyncio module, where they stay (a bot can use them too)."""
import asyncio
import socket
import struct
import time

from . import _compat

CancelledError = asyncio.CancelledError
FIRST_COMPLETED = "FIRST_COMPLETED"
FIRST_EXCEPTION = "FIRST_EXCEPTION"
ALL_COMPLETED = "ALL_COMPLETED"


class InvalidStateError(Exception):
    pass


class QueueEmpty(Exception):
    pass


class QueueFull(Exception):
    pass


class Future:
    def __init__(self):
        self._state = 0  # 1 done, 2 cancelled
        self._result = None
        self._exc = None
        self._ev = asyncio.Event()
        self._callbacks = []

    def done(self):
        return self._state != 0

    def cancelled(self):
        return self._state == 2

    def result(self):
        if self._state == 2:
            raise CancelledError
        if self._state == 0:
            raise InvalidStateError("Result is not set.")
        if self._exc is not None:
            raise self._exc
        return self._result

    def exception(self):
        if self._state == 2:
            raise CancelledError
        if self._state == 0:
            raise InvalidStateError("Exception is not set.")
        return self._exc

    def _finish(self, state):
        if self._state:
            raise InvalidStateError("invalid state")
        self._state = state
        self._ev.set()
        cbs, self._callbacks = self._callbacks, []
        for cb in cbs:
            cb(self)

    def set_result(self, result):
        if self._state:
            raise InvalidStateError("invalid state")
        self._result = result
        self._finish(1)

    def set_exception(self, exc):
        if self._state:
            raise InvalidStateError("invalid state")
        self._exc = exc
        self._finish(1)

    def cancel(self, msg=None):
        if self._state:
            return False
        self._finish(2)
        return True

    def add_done_callback(self, cb, **kw):
        if self._state:
            cb(self)
        else:
            self._callbacks.append(cb)

    def remove_done_callback(self, cb):
        n = len(self._callbacks)
        self._callbacks = [c for c in self._callbacks if c != cb]
        return n - len(self._callbacks)

    # await future (MicroPython awaits through __iter__, not __await__)
    def __iter__(self):
        if not self._state:
            yield from self._ev.wait()
        return self.result()

    __await__ = __iter__


async def _future_result(f):
    try:
        return await f
    except CancelledError:
        f.cancel()  # as CPython's wait_for cancels what it waits for
        raise


_wait_for = asyncio.wait_for


def wait_for(aw, timeout):
    if isinstance(aw, Future):
        aw = _future_result(aw)
    return _wait_for(aw, timeout)


# wait() must not await a task: cancelling a task that awaits another
# cancels that one too in MicroPython. It takes the hook gather() uses
# instead, a callable in the task's state, called when the task ends;
# _task_hook passes that on to every wait() that is waiting for the task.
_task_cbs = {}


def _task_hook(t, er):
    for cb in _task_cbs.pop(t, ()):
        cb(t, er)


def _on_done(f, cb):
    """cb(f, exception or None) when f ends. Returns a function that undoes
    this, or None when f is a task that something already awaits."""
    if isinstance(f, Future):
        g = lambda f: cb(f, None if f.cancelled() else f.exception())  # noqa: E731
        f.add_done_callback(g)
        return lambda: f.remove_done_callback(g)
    if f.state is not True and f.state is not _task_hook:
        return None
    f.state = _task_hook
    _task_cbs.setdefault(f, []).append(cb)

    def undo():
        cbs = _task_cbs.get(f)
        if cbs and cb in cbs:
            cbs.remove(cb)
        if not cbs and f.state is _task_hook:
            _task_cbs.pop(f, None)
            f.state = True  # running, nothing waiting: awaitable again
    return undo


async def wait(fs, timeout=None, return_when=ALL_COMPLETED):
    fs = set(fs)
    if not fs:
        raise ValueError("Set of Tasks/Futures is empty.")
    ev = asyncio.Event()
    failed = []

    def ready():
        n = sum(1 for f in fs if f.done())
        if return_when == FIRST_COMPLETED:
            return n > 0
        if return_when == FIRST_EXCEPTION and failed:
            return True
        return n == len(fs)

    def ended(f, er):
        if er is not None and not isinstance(er, StopIteration):
            failed.append(f)
        ev.set()

    undo, poll = [], False
    try:
        for f in fs:
            if not f.done():
                u = _on_done(f, ended)
                if u is None:
                    poll = True
                else:
                    undo.append(u)

        async def until_ready():
            while not ready():
                ev.clear()
                if poll:
                    await asyncio.sleep_ms(10)
                else:
                    await ev.wait()

        if not ready():
            try:
                if timeout is None:
                    await until_ready()
                else:
                    await _wait_for(until_ready(), timeout)
            except asyncio.TimeoutError:
                pass
    finally:
        for u in undo:
            u()
    done = set(f for f in fs if f.done())
    return done, fs - done


class Queue:
    def __init__(self, maxsize=0):
        self.maxsize = maxsize
        self._items = []
        self._unfinished = 0
        self._got = asyncio.Event()  # an item was taken
        self._put = asyncio.Event()  # an item was added
        self._all_done = asyncio.Event()
        self._all_done.set()

    def qsize(self):
        return len(self._items)

    def empty(self):
        return not self._items

    def full(self):
        return 0 < self.maxsize <= len(self._items)

    def put_nowait(self, item):
        if self.full():
            raise QueueFull
        self._items.append(item)
        self._unfinished += 1
        self._all_done.clear()
        self._put.set()

    async def put(self, item):
        while self.full():
            self._got.clear()
            await self._got.wait()
        self.put_nowait(item)

    def get_nowait(self):
        if not self._items:
            raise QueueEmpty
        self._got.set()
        return self._items.pop(0)

    async def get(self):
        while not self._items:
            self._put.clear()
            await self._put.wait()
        return self.get_nowait()

    def task_done(self):
        if self._unfinished <= 0:
            raise ValueError("task_done() called too many times")
        self._unfinished -= 1
        if not self._unfinished:
            self._all_done.set()

    async def join(self):
        if self._unfinished:
            await self._all_done.wait()


class Protocol:
    def connection_made(self, transport):
        pass

    def data_received(self, data):
        pass

    def eof_received(self):
        pass

    def connection_lost(self, exc):
        pass


class _Transport:
    """A stream socket as an asyncio transport: data to the protocol's
    data_received() until the other end closes or close() is called."""

    def __init__(self, sock, protocol):
        self._sock = sock
        self._stream = asyncio.StreamReader(sock)
        self._protocol = protocol
        self._closing = False
        self._lost = False
        protocol.connection_made(self)
        self._task = asyncio.create_task(self._pump())

    async def _pump(self):
        exc = None
        try:
            while True:
                data = await self._stream.read(512)
                if not data:
                    break
                self._protocol.data_received(data)
        except CancelledError:
            pass
        except Exception as e:
            exc = e
        self._close()
        self._connection_lost(exc)

    def _connection_lost(self, exc):
        if not self._lost:
            self._lost = True
            self._protocol.connection_lost(exc)

    def _close(self):
        if self._sock is not None:
            self._sock.close()
            self._sock = None

    def write(self, data):
        if self._sock is None:
            raise OSError(9)  # EBADF
        self._stream.write(data)
        if self._stream.out_buf:
            asyncio.create_task(self._stream.drain())

    def is_closing(self):
        return self._closing

    def get_extra_info(self, name, default=None):
        return default

    def close(self):
        if not self._closing:
            self._closing = True
            self._task.cancel()
            self._close()
            self._connection_lost(None)

    abort = close


def _writable(s):
    yield asyncio.core._io_queue.queue_write(s)


def _unix_address(path):
    # struct sockaddr_un: sun_family (native order) and the path
    return struct.pack("H", socket.AF_UNIX) + path.encode() + b"\0"


class _Loop:
    def create_future(self):
        return Future()

    def create_task(self, coro):
        return asyncio.create_task(coro)

    def time(self):
        return time.ticks_ms() / 1000

    async def create_connection(self, protocol_factory, host=None, port=None, **kw):
        ai = socket.getaddrinfo(host, port, 0, socket.SOCK_STREAM)[0]
        s = socket.socket(ai[0], ai[1], ai[2])
        s.setblocking(False)
        try:
            s.connect(ai[-1])
        except OSError as e:
            if e.errno != 115:  # EINPROGRESS
                s.close()
                raise
        await _writable(s)  # once connected
        p = protocol_factory()
        return _Transport(s, p), p

    async def create_unix_connection(self, protocol_factory, path=None, **kw):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.connect(_unix_address(path))  # a local socket: no waiting
        except OSError:
            s.close()
            raise
        s.setblocking(False)
        p = protocol_factory()
        return _Transport(s, p), p


_loop = _Loop()


def get_running_loop():
    return _loop


def install():
    a = asyncio
    a.Future = Future
    a.Queue = Queue
    a.QueueEmpty = QueueEmpty
    a.QueueFull = QueueFull
    a.InvalidStateError = InvalidStateError
    a.Protocol = Protocol
    a.FIRST_COMPLETED = FIRST_COMPLETED
    a.FIRST_EXCEPTION = FIRST_EXCEPTION
    a.ALL_COMPLETED = ALL_COMPLETED
    a.wait = wait
    a.wait_for = wait_for
    a.get_running_loop = get_running_loop
    a.iscoroutinefunction = _compat.iscoroutinefunction
    a.iscoroutine = _compat.iscoroutine
