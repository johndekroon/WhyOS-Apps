# SPDX-License-Identifier: MIT
"""A small logging for MicroPython, as CPython's as far as meshcore_py and a
bot need it: getLogger, the levels, basicConfig, handlers, and the
exc_info=True that the library passes (micropython-lib's logging takes no
such argument). meshcore/__init__.py installs it as logging when there is
none."""
import sys
import time

CRITICAL = 50
ERROR = 40
WARNING = 30
INFO = 20
DEBUG = 10
NOTSET = 0

_names = {CRITICAL: "CRITICAL", ERROR: "ERROR", WARNING: "WARNING", INFO: "INFO", DEBUG: "DEBUG", NOTSET: "NOTSET"}


def getLevelName(level):
    return _names.get(level, "Level %s" % level)


def addLevelName(level, name):
    _names[level] = name


class LogRecord:
    def __init__(self, name, level, msg, args, exc):
        self.name = name
        self.levelno = level
        self.levelname = getLevelName(level)
        self.msg = msg
        self.args = args
        self.exc = exc
        self.created = time.time()

    def getMessage(self):
        msg = str(self.msg)
        if self.args:
            msg = msg % self.args
        return msg


class Formatter:
    def __init__(self, fmt=None, datefmt=None, style="%"):
        self.fmt = fmt or "%(levelname)s:%(name)s:%(message)s"
        self.datefmt = datefmt

    def format(self, record):
        record.message = record.getMessage()
        if "%(asctime)" in self.fmt:
            t = time.localtime(int(record.created))
            record.asctime = "%04d-%02d-%02d %02d:%02d:%02d" % t[:6]
        s = self.fmt % record.__dict__
        if record.exc is not None:
            b = __import__("io").StringIO()
            sys.print_exception(record.exc, b)
            s += "\n" + b.getvalue().rstrip("\n")
        return s


_default = Formatter()


class Handler:
    def __init__(self, level=NOTSET):
        self.level = level
        self.formatter = None

    def setLevel(self, level):
        self.level = level

    def setFormatter(self, fmt):
        self.formatter = fmt

    def format(self, record):
        return (self.formatter or _default).format(record)

    def handle(self, record):
        if record.levelno >= self.level:
            self.emit(record)

    def emit(self, record):
        pass

    def flush(self):
        pass

    def close(self):
        pass


class NullHandler(Handler):
    pass


class StreamHandler(Handler):
    def __init__(self, stream=None):
        super().__init__()
        self.stream = stream

    def emit(self, record):
        s = self.stream or sys.stderr
        s.write(self.format(record) + "\n")


class FileHandler(StreamHandler):
    def __init__(self, filename, mode="a", encoding="UTF-8"):
        super().__init__(open(filename, mode))

    def emit(self, record):
        super().emit(record)
        self.stream.flush()

    def close(self):
        self.stream.close()


class Logger:
    def __init__(self, name, parent=None):
        self.name = name
        self.level = NOTSET
        self.handlers = []
        self.parent = parent
        self.propagate = True
        self.disabled = False

    def setLevel(self, level):
        self.level = level

    def getEffectiveLevel(self):
        lg = self
        while lg is not None:
            if lg.level:
                return lg.level
            lg = lg.parent
        return NOTSET

    def isEnabledFor(self, level):
        return level >= self.getEffectiveLevel()

    def addHandler(self, h):
        if h not in self.handlers:
            self.handlers.append(h)

    def removeHandler(self, h):
        if h in self.handlers:
            self.handlers.remove(h)

    def hasHandlers(self):
        lg = self
        while lg is not None:
            if lg.handlers:
                return True
            lg = lg.parent if lg.propagate else None
        return False

    def getChild(self, suffix):
        return getLogger(self.name + "." + suffix if self is not root else suffix)

    def log(self, level, msg, *args, exc_info=None, **kw):
        if self.disabled or not self.isEnabledFor(level):
            return
        exc = None
        if exc_info:
            exc = exc_info if isinstance(exc_info, BaseException) else sys.exc_info()[1]
        r = LogRecord(self.name, level, msg, args, exc)
        found = False
        lg = self
        while lg is not None:
            for h in lg.handlers:
                found = True
                h.handle(r)
            lg = lg.parent if lg.propagate else None
        if not found and level >= WARNING:
            # CPython's last resort: warnings and worse to stderr
            sys.stderr.write(r.getMessage() + "\n")

    def debug(self, msg, *args, **kw):
        self.log(DEBUG, msg, *args, **kw)

    def info(self, msg, *args, **kw):
        self.log(INFO, msg, *args, **kw)

    def warning(self, msg, *args, **kw):
        self.log(WARNING, msg, *args, **kw)

    warn = warning

    def error(self, msg, *args, **kw):
        self.log(ERROR, msg, *args, **kw)

    def exception(self, msg, *args, exc_info=True, **kw):
        self.log(ERROR, msg, *args, exc_info=exc_info, **kw)

    def critical(self, msg, *args, **kw):
        self.log(CRITICAL, msg, *args, **kw)


root = Logger("root")
root.level = WARNING
_loggers = {}


def getLogger(name=None):
    if not name or name == "root":
        return root
    lg = _loggers.get(name)
    if lg is None:
        dot = name.rfind(".")
        parent = getLogger(name[:dot]) if dot > 0 else root
        lg = _loggers[name] = Logger(name, parent)
    return lg


def basicConfig(level=None, format=None, datefmt=None, stream=None, filename=None, filemode="a", force=False, **kw):
    if root.handlers and not force:
        return
    for h in root.handlers:
        h.close()
    h = FileHandler(filename, filemode) if filename else StreamHandler(stream)
    h.setFormatter(Formatter(format, datefmt))
    root.handlers = [h]
    if level is not None:
        root.setLevel(level)


def debug(msg, *args, **kw):
    root.debug(msg, *args, **kw)


def info(msg, *args, **kw):
    root.info(msg, *args, **kw)


def warning(msg, *args, **kw):
    root.warning(msg, *args, **kw)


def error(msg, *args, **kw):
    root.error(msg, *args, **kw)


def exception(msg, *args, **kw):
    root.exception(msg, *args, **kw)


def critical(msg, *args, **kw):
    root.critical(msg, *args, **kw)
