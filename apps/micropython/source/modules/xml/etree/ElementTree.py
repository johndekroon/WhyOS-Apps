# SPDX-License-Identifier: MIT
"""XML as a tree of elements: CPython's xml.etree.ElementTree for
MicroPython, written anew in Python (CPython's reads XML with expat, a C
library MicroPython does not have).

    import xml.etree.ElementTree as ET
    root = ET.fromstring("<a><b x='1'>hi</b></a>")
    root.find("b").get("x"), root.findtext("b")
    for event, elem in ET.iterparse(path_or_file):  # in pieces, for big files
        ...
        elem.clear()

There: Element, SubElement, Comment, ProcessingInstruction (PI),
ElementTree, parse, fromstring (XML), fromstringlist, iterparse,
XMLPullParser, XMLParser (with your own target), TreeBuilder, tostring,
tostringlist, dump, indent, iselement, register_namespace, ParseError.
find/findall/findtext/iterfind take the same paths as CPython: tag, *,
., .., //, {uri}tag, {*}tag, prefix:tag with namespaces=, [@a], [@a='v'],
[@a!='v'], [tag], [tag='text'], [.='text'], [n], [last()], [last()-n].

Input in UTF-8, US-ASCII, ISO-8859-1 or Windows-1252, as bytes or str.
Namespaces become {uri}tag, as in CPython. A DOCTYPE is skipped: entities
it declares are unknown (with an external DTD they come from
XMLParser().entity, as in CPython). Output in "unicode", "utf-8" or
"us-ascii", method "xml" or "text".

Unlike CPython: attrib is an OrderedDict, so that attributes keep their
order (a MicroPython dict has none); MicroPython compares it with a dict
in order too, so compare dict(e.attrib). While reading, garbage is
collected every 256 KB allocated (see _GC_EVERY).
"""

try:
    from collections import OrderedDict as _odict  # attributes in document order
except ImportError:
    _odict = dict

VERSION = "1.3.0"
_CHUNK = 4096  # read at a time; small pieces leave the heap less fragmented
# While reading: collect garbage after this many bytes allocated. Else the
# heap fills up before each collection and the tree's elements end up
# spread over all of it, with no room left between them for a long text
# or list (a 2 MB heap failed on a 250 KB file, and in iterparse() on a
# 1.9 MB file with a 30 KB text).
_GC_EVERY = 262144


def _gc_often():
    # the threshold to put back afterwards, or None
    try:
        import gc
        old = gc.threshold()
    except (ImportError, AttributeError):  # not MicroPython
        return None
    if 0 <= old <= _GC_EVERY:
        return None  # often enough already
    gc.threshold(_GC_EVERY)
    return old


def _gc_restore(old):
    if old is not None:
        import gc
        gc.threshold(old)


class ParseError(SyntaxError):
    pass


def iselement(element):
    return hasattr(element, "tag")


def _check(e):
    if not isinstance(e, Element):
        raise TypeError("expected an Element, not %s" % type(e).__name__)


class Element:
    def __init__(self, tag, attrib={}, **extra):
        if not isinstance(attrib, dict):
            raise TypeError("attrib must be dict, not %s" % type(attrib).__name__)
        self.tag = tag
        self.attrib = _odict(attrib)
        if extra:
            self.attrib.update(extra)
        self.text = None
        self.tail = None
        self._children = []

    def __repr__(self):
        return "<Element %r at %#x>" % (self.tag, id(self))

    def makeelement(self, tag, attrib):
        return self.__class__(tag, attrib)

    def __len__(self):
        return len(self._children)

    def __getitem__(self, index):
        return self._children[index]

    def __setitem__(self, index, element):
        if isinstance(index, int):
            _check(element)
        else:
            element = list(element)
            for e in element:
                _check(e)
        self._children[index] = element

    def __delitem__(self, index):
        del self._children[index]

    def __iter__(self):
        return iter(self._children)

    def append(self, subelement):
        _check(subelement)
        self._children.append(subelement)

    def extend(self, elements):
        for e in elements:
            _check(e)
            self._children.append(e)

    def insert(self, index, subelement):
        _check(subelement)
        self._children.insert(index, subelement)

    def remove(self, subelement):
        self._children.remove(subelement)

    # find, findtext and findall take a path without / * [ @ . (outside
    # {uri}) and without namespaces= as a plain tag, as CPython does
    def find(self, path, namespaces=None):
        for e in _select(self, path, namespaces):
            return e
        return None

    def findtext(self, path, default=None, namespaces=None):
        for e in _select(self, path, namespaces):
            return e.text or ""
        return default

    def findall(self, path, namespaces=None):
        return list(_select(self, path, namespaces))

    def iterfind(self, path, namespaces=None):
        return _iterfind(self, path, namespaces)

    def clear(self):
        self.attrib.clear()
        self._children = []
        self.text = self.tail = None

    def get(self, key, default=None):
        return self.attrib.get(key, default)

    def set(self, key, value):
        self.attrib[key] = value

    def keys(self):
        return self.attrib.keys()

    def items(self):
        return self.attrib.items()

    def iter(self, tag=None):
        # in document order, without recursion
        if tag == "*":
            tag = None
        if tag is None or self.tag == tag:
            yield self
        stack = [iter(self._children)]
        while stack:
            for e in stack[-1]:
                if tag is None or e.tag == tag:
                    yield e
                if e._children:
                    stack.append(iter(e._children))
                    break
            else:
                stack.pop()

    def itertext(self):
        # comments and processing instructions too, as CPython's C version
        if self.text:
            yield self.text
        for e in self._children:
            yield from e.itertext()
            if e.tail:
                yield e.tail


def SubElement(parent, tag, attrib={}, **extra):
    e = parent.makeelement(tag, dict(attrib, **extra))
    parent.append(e)
    return e


def Comment(text=None):
    e = Element(Comment)
    e.text = text
    return e


def ProcessingInstruction(target, text=None):
    e = Element(ProcessingInstruction)
    e.text = target + " " + text if text else target
    return e


PI = ProcessingInstruction


class ElementTree:
    def __init__(self, element=None, file=None):
        self._root = element
        if file is not None:
            self.parse(file)

    def getroot(self):
        return self._root

    def _setroot(self, element):
        self._root = element

    def parse(self, source, parser=None):
        f = open(source, "rb") if not hasattr(source, "read") else source
        old = _gc_often()
        try:
            if parser is None:
                parser = XMLParser()
            while True:
                data = f.read(_CHUNK)
                if not data:
                    break
                parser.feed(data)
            self._root = parser.close()
            return self._root
        finally:
            _gc_restore(old)
            if f is not source:
                f.close()

    def iter(self, tag=None):
        return self._root.iter(tag)

    def _path(self, path):
        return "." + path if path[:1] == "/" else path

    def find(self, path, namespaces=None):
        return self._root.find(self._path(path), namespaces)

    def findtext(self, path, default=None, namespaces=None):
        return self._root.findtext(self._path(path), default, namespaces)

    def findall(self, path, namespaces=None):
        return self._root.findall(self._path(path), namespaces)

    def iterfind(self, path, namespaces=None):
        return self._root.iterfind(self._path(path), namespaces)

    def write(self, file_or_filename, encoding=None, xml_declaration=None,
              default_namespace=None, method=None, *, short_empty_elements=True):
        out = _serialize(self._root, encoding, xml_declaration, default_namespace,
                         method, short_empty_elements)
        if hasattr(file_or_filename, "write"):
            file_or_filename.write(out)
        else:
            with open(file_or_filename, "w" if isinstance(out, str) else "wb") as f:
                f.write(out)


def parse(source, parser=None):
    tree = ElementTree()
    tree.parse(source, parser)
    return tree


def XML(text, parser=None):
    return fromstringlist((text,), parser)


fromstring = XML


def fromstringlist(sequence, parser=None):
    if parser is None:
        parser = XMLParser()
    old = _gc_often()
    try:
        for text in sequence:
            parser.feed(text)
        return parser.close()
    finally:
        _gc_restore(old)


# --- paths (find, findall, ...) -----------------------------------------


def _tokens(path, namespaces):
    # (op, tag) pairs, as CPython's ElementPath
    default = namespaces.get("") if namespaces else None
    attr = False
    i, n = 0, len(path)
    while i < n:
        c = path[i]
        two = path[i:i + 2]
        tag = ""
        if c in " \t\n\r":
            i += 1
            continue
        if c == "'" or c == '"':
            j = path.find(c, i + 1)
            if j < 0:
                raise SyntaxError("invalid path")
            op = path[i:j + 1]
            i = j + 1
        elif two in ("::", "//", "..", "()", "!="):
            op = two
            i += 2
        elif c in "/.*:[]()@=":
            op = c
            i += 1
        else:
            op = ""
            j = i
            if c == "{":
                k = path.find("}", i + 2)
                if k > 0:
                    j = k + 1
            while j < n and path[j] not in "/[]()@!= \t\n\r":
                j += 1
            tag = path[i:j]
            i = j
            if tag[0] != "{":
                if ":" in tag:
                    prefix, local = tag.split(":", 1)
                    if not namespaces or prefix not in namespaces:
                        raise SyntaxError("prefix %r not found in prefix map" % prefix)
                    tag = "{%s}%s" % (namespaces[prefix], local)
                elif default and not attr:
                    tag = "{%s}%s" % (default, tag)
        attr = op == "@"
        yield op, tag


def _wild(tag):
    return tag[:3] == "{*}" or tag[-2:] == "}*"


def _tag_test(tag):
    # a test for a tag with {*} or }*
    if tag == "{*}*":
        return lambda t: isinstance(t, str)
    if tag == "{}*":
        return lambda t: isinstance(t, str) and t[:1] != "{"
    if tag[:3] == "{*}":
        local, suffix = tag[3:], tag[2:]
        return lambda t: t == local or isinstance(t, str) and t.endswith(suffix)
    ns = tag[:-1]
    return lambda t: isinstance(t, str) and t.startswith(ns)


def _children_named(tag):
    if _wild(tag):
        test = _tag_test(tag)

        def select(ctx, result):
            for elem in result:
                for e in elem:
                    if test(e.tag):
                        yield e
        return select
    if tag[:2] == "{}":
        tag = tag[2:]

    def select(ctx, result):
        for elem in result:
            for e in elem:
                if e.tag == tag:
                    yield e
    return select


def _all_children(ctx, result):
    for elem in result:
        yield from elem


def _self(ctx, result):
    yield from result


def _descendants(tag):
    if tag == "*":
        tag = None
    test = None
    if tag is not None and _wild(tag):
        test, tag = _tag_test(tag), None
    elif tag is not None and tag[:2] == "{}":
        tag = tag[2:]

    def select(ctx, result):
        for elem in result:
            for e in elem.iter(tag):
                if e is not elem and (test is None or test(e.tag)):
                    yield e
    return select


def _parents(ctx):
    if ctx[1] is None:
        ctx[1] = {e: p for p in ctx[0].iter() for e in p}
    return ctx[1]


def _parent(ctx, result):
    pmap = _parents(ctx)
    seen = {}
    for elem in result:
        p = pmap.get(elem)
        if p is not None and p not in seen:
            seen[p] = None
            yield p


def _isint(s):
    return s[1:].isdigit() if s[:1] == "-" else s.isdigit()


def _text(e):
    return "".join(e.itertext())


def _predicate(nxt):
    sig, args = "", []
    while True:
        op, tag = nxt()
        if op == "]":
            break
        if op[:1] in ("'", '"'):
            op, tag = "'", op[1:-1]
        sig += op or "-"
        args.append(tag)
    if sig == "@-":
        key = args[1]
        return lambda ctx, result: (e for e in result if e.get(key) is not None)
    if sig in ("@-='", "@-!='"):
        key, value = args[1], args[-1]
        if "!" in sig:
            return lambda ctx, result: (e for e in result
                                        if e.get(key) is not None and e.get(key) != value)
        return lambda ctx, result: (e for e in result if e.get(key) == value)
    if sig == "-" and not _isint(args[0]):
        tag = args[0]
        return lambda ctx, result: (e for e in result if e.find(tag) is not None)
    if sig in (".='", ".!='") or sig in ("-='", "-!='") and not _isint(args[0]):
        tag, value, neg = args[0], args[-1], "!" in sig
        if tag:
            def select(ctx, result):
                for elem in result:
                    for e in elem.iterfind(tag):
                        if (_text(e) != value) == neg:
                            yield elem
                            break
            return select
        return lambda ctx, result: (e for e in result if (_text(e) != value) == neg)
    if sig in ("-", "-()", "-()-"):
        if sig == "-":
            index = int(args[0]) - 1
            if index < 0:
                raise SyntaxError("XPath position >= 1 expected")
        else:
            if args[0] != "last":
                raise SyntaxError("unsupported function")
            index = -1
            if sig == "-()-":
                try:
                    index = int(args[2]) - 1
                except ValueError:
                    raise SyntaxError("unsupported expression")
                if index > -2:
                    raise SyntaxError("XPath offset from last() must be negative")

        def select(ctx, result):
            pmap = _parents(ctx)
            for elem in result:
                p = pmap.get(elem)
                if p is not None:
                    same = p.findall(elem.tag)
                    if -len(same) <= index < len(same) and same[index] is elem:
                        yield elem
        return select
    raise SyntaxError("invalid predicate")


_cache = {}


def _compile(path, namespaces):
    toks = _tokens(path, namespaces)

    def nxt():
        return next(toks)

    sel = []
    try:
        op, tag = nxt()
    except StopIteration:
        return sel
    while True:
        try:
            if op == "":
                sel.append(_children_named(tag))
            elif op == "*":
                sel.append(_all_children)
            elif op == ".":
                sel.append(_self)
            elif op == "..":
                sel.append(_parent)
            elif op == "//":
                op, tag = nxt()
                if op == "*":
                    tag = "*"
                elif op:
                    raise SyntaxError("invalid descendant")
                sel.append(_descendants(tag))
            elif op == "[":
                sel.append(_predicate(nxt))
            else:
                raise SyntaxError("invalid path")
        except StopIteration:
            raise SyntaxError("invalid path")
        try:
            op, tag = nxt()
            if op == "/":
                op, tag = nxt()
        except StopIteration:
            return sel


def _select(elem, path, namespaces):
    # a plain tag: the children with it; else the path
    if namespaces is None and isinstance(path, str) and path[:3] != "{*}" and path[:2] != "{}":
        inside = False
        for c in path:
            if c == "{":
                inside = True
            elif c == "}":
                inside = False
            elif not inside and c in "/*[@.":
                break
        else:
            return [e for e in elem._children if e.tag == path]
    return _iterfind(elem, path, namespaces)


def _iterfind(elem, path, namespaces=None):
    if path[-1:] == "/":
        path += "*"
    key = (path, tuple(sorted(namespaces.items())) if namespaces else None)
    sel = _cache.get(key)
    if sel is None:
        if path[:1] == "/":
            raise SyntaxError("cannot use absolute path on element")
        sel = _compile(path, namespaces)
        if len(_cache) > 50:
            _cache.clear()
        _cache[key] = sel
    result = iter((elem,))
    ctx = [elem, None]  # the root and, once needed, a child -> parent map
    for select in sel:
        result = select(ctx, result)
    return result


# --- reading ------------------------------------------------------------


class TreeBuilder:
    def __init__(self, element_factory=None, *, comment_factory=None, pi_factory=None,
                 insert_comments=False, insert_pis=False):
        self._factory = element_factory or Element
        self._comment_factory = comment_factory or Comment
        self._pi_factory = pi_factory or ProcessingInstruction
        self.insert_comments = insert_comments
        self.insert_pis = insert_pis
        self._data = []
        self._elem = []
        self._last = None
        self._root = None
        self._tail = False

    def close(self):
        assert not self._elem, "missing end tags"
        assert self._root is not None, "missing toplevel element"
        return self._root

    def _flush(self):
        if self._data:
            if self._last is not None:
                text = "".join(self._data)
                if self._tail:
                    self._last.tail = text
                else:
                    self._last.text = text
            self._data = []

    def data(self, data):
        self._data.append(data)

    def start(self, tag, attrs):
        self._flush()
        if self._factory is Element and type(attrs) is _odict:
            # from XMLParser: a new dict, no need to copy it
            e = object.__new__(Element)
            e.tag, e.attrib, e.text, e.tail, e._children = tag, attrs, None, None, []
        else:
            e = self._factory(tag, attrs)
        self._last = e
        if self._elem:
            self._elem[-1].append(e)
        elif self._root is None:
            self._root = e
        self._elem.append(e)
        self._tail = False
        return e

    def end(self, tag):
        self._flush()
        self._last = self._elem.pop()
        self._tail = True
        return self._last

    def _single(self, e, insert):
        if insert:
            self._flush()
            self._last = e
            if self._elem:
                self._elem[-1].append(e)
            self._tail = True
        return e

    def comment(self, text):
        return self._single(self._comment_factory(text), self.insert_comments)

    def pi(self, target, text=None):
        return self._single(self._pi_factory(target, text), self.insert_pis)


class _Bad(Exception):
    pass


_ENT = {"amp": "&", "lt": "<", "gt": ">", "quot": '"', "apos": "'"}
_XML_NS = "http://www.w3.org/XML/1998/namespace"
_WS = b" \t\n"
_NAME_END = b" \t\n/>="
_BAD_NAME = "<>&\"'=/!?;"


def _utf8(b):
    try:
        return str(b, "utf-8")
    except UnicodeError:
        raise _Bad("not well-formed (invalid token)")


def _latin1(b):
    for c in b:
        if c > 127:
            return "".join([chr(c) for c in b])
    return str(b, "utf-8")


_CP1252 = ("\u20ac\x81\u201a\u0192\u201e\u2026\u2020\u2021\u02c6\u2030\u0160\u2039\u0152\x8d\u017d\x8f"
           "\x90\u2018\u2019\u201c\u201d\u2022\u2013\u2014\u02dc\u2122\u0161\u203a\u0153\x9d\u017e\u0178")


def _cp1252(b):
    for c in b:
        if c > 127:
            return "".join([_CP1252[c - 128] if 127 < c < 160 else chr(c) for c in b])
    return str(b, "utf-8")


_DECODERS = {"utf-8": _utf8, "utf8": _utf8, "us-ascii": _utf8, "ascii": _utf8,
             "iso-8859-1": _latin1, "iso8859-1": _latin1, "latin-1": _latin1,
             "latin1": _latin1, "l1": _latin1, "windows-1252": _cp1252, "cp1252": _cp1252}


class XMLParser:
    """Reads XML fed to it in pieces of any size and calls the target's
    start(tag, attrib), end(tag), data(text), comment(text), pi(target,
    text), start_ns(prefix, uri), end_ns(prefix) and close(), each if
    the target has it."""

    def __init__(self, *, target=None, encoding=None):
        if target is None:
            target = TreeBuilder()
        self.target = target
        self.entity = {}
        self.version = "MicroPython ElementTree " + VERSION
        for name in ("start", "end", "data", "comment", "pi", "start_ns", "end_ns", "close"):
            setattr(self, "_t_" + name, getattr(target, name, None))
        self._dec = None
        if encoding:
            self._dec = _DECODERS.get(encoding.lower())
            if self._dec is None:
                raise LookupError("unknown encoding: %s" % encoding)
        self._buf = b""
        self._cr = False
        self._lines = 0  # newlines before _buf
        self._col = 0  # column where _buf starts
        self._stack = []  # (raw name, tag, declared prefixes) per open element
        self._map = {"xml": _XML_NS}  # prefix -> uri; "" the default
        self._names = {}  # raw element name -> tag, for this _map
        self._anames = {}  # the same for attribute names
        self._maps = []
        self._started = self._ended = self._closed = False
        self._dtd = False  # an external DTD: entities from self.entity
        self._events = None
        self._ev_start = self._ev_end = False

    def _setevents(self, queue, events):
        want = []
        for ev in events:
            if ev not in ("start", "end", "start-ns", "end-ns", "comment", "pi"):
                raise ValueError("unknown event %r" % ev)
            want.append(ev)
        self._events = queue
        self._want = want
        self._ev_start = "start" in want
        self._ev_end = "end" in want

    def feed(self, data):
        if self._closed:
            raise ParseError("feed() after close()")
        if isinstance(data, str):
            data = data.encode()
            if self._dec is None:
                self._dec = _utf8  # a str is already decoded: no declared encoding
        elif not isinstance(data, bytes):
            data = bytes(data)
        if not data:
            return
        # line ends as \n, also \r\n split over two pieces
        if self._cr:
            data = b"\r" + data
            self._cr = False
        if data[-1] == 13:
            data = data[:-1]
            self._cr = True
        if b"\r" in data:
            data = data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
        self._buf = self._buf + data if self._buf else data
        self._scan(False)

    def close(self):
        if self._cr:
            self._buf += b"\n"
            self._cr = False
        self._scan(True)
        if self._stack or not self._started:
            raise self._error("no element found" if not self._started else "unclosed token",
                              self._buf, len(self._buf))
        self._closed = True
        if self._t_close:
            return self._t_close()

    def _error(self, msg, b, pos):
        line = self._lines + b.count(b"\n", 0, pos) + 1
        nl = b.rfind(b"\n", 0, pos)
        col = pos - nl - 1 if nl >= 0 else self._col + pos
        err = ParseError("%s: line %d, column %d" % (msg, line, col))
        err.position = (line, col)
        err.code = 0
        return err

    def _encoding(self, b, final):
        # the encoding from a BOM or <?xml ... encoding="..."?>; False: need more
        if len(b) < 5 and not final:
            return False
        if b[:3] == b"\xef\xbb\xbf":
            self._buf = b[3:]
            return _utf8
        if b[:2] in (b"\xff\xfe", b"\xfe\xff") or b[:1] == b"\0" or b[1:2] == b"\0":
            raise self._error("UTF-16 is not supported", b, 0)
        if b[:5] != b"<?xml":
            return _utf8
        e = b.find(b"?>")
        if e < 0:
            if not final:
                return False
            raise self._error("unclosed token", b, 0)
        k = b.find(b"encoding", 0, e)
        if k < 0:
            return _utf8
        v = b[k + 8:e].lstrip().lstrip(b"=").lstrip()
        name = str(v[1:v.find(v[:1], 1)], "utf-8").lower()
        if name not in _DECODERS:
            raise self._error("unknown encoding: " + name, b, k)
        return _DECODERS[name]

    def _ents(self, s):
        parts = s.split("&")
        out = [parts[0]]
        for p in parts[1:]:
            k = p.find(";")
            if k < 1:
                raise _Bad("not well-formed (invalid token)")
            name = p[:k]
            if name[0] == "#":
                num = name[2:] if name[1:2] == "x" else name[1:]
                if not num or not (num.isdigit() or name[1:2] == "x" and
                                   not num.lower().strip("0123456789abcdef")):
                    raise _Bad("not well-formed (invalid token)")
                c = int(num, 16 if name[1:2] == "x" else 10)
                if not (c in (9, 10, 13) or 32 <= c <= 0xD7FF or 0xE000 <= c <= 0xFFFD or
                        0x10000 <= c <= 0x10FFFF):
                    raise _Bad("reference to invalid character number")
                out.append(chr(c))
            else:
                v = _ENT.get(name)
                if v is None:
                    v = self.entity.get(name) if self._dtd else None
                    if v is None:
                        raise _Bad("undefined entity &%s;" % name)
                out.append(v)
            out.append(p[k + 1:])
        return "".join(out)

    def _qname(self, raw, elem):
        s = self._dec(raw)
        if not s or s[0] in "0123456789-." or any(c in _BAD_NAME for c in s):
            raise _Bad("not well-formed (invalid token)")
        k = s.find(":")
        if k >= 0:
            uri = self._map.get(s[:k])
            if not uri or k == 0:
                raise _Bad("unbound prefix")
            return "{%s}%s" % (uri, s[k + 1:])
        if elem:
            uri = self._map.get("")
            if uri:
                return "{%s}%s" % (uri, s)
        return s

    def _value(self, raw):
        v = self._dec(raw)
        if "<" in v:
            raise _Bad("not well-formed (invalid token)")
        if "\t" in v or "\n" in v:
            v = v.replace("\t", " ").replace("\n", " ")
        if "&" in v:
            v = self._ents(v)
        return v

    def _start(self, raw, attrs):
        # attrs: [(raw name, raw value)]
        decl = None
        if attrs:
            plain = []
            for an, av in attrs:
                if an[:5] == b"xmlns" and (len(an) == 5 or an[5] == 58):
                    if decl is None:
                        decl = []
                    decl.append((self._dec(an[6:]), self._value(av)))
                else:
                    plain.append((an, av))
            attrs = plain
        if decl:
            self._maps.append((self._map, self._names, self._anames))
            self._map = dict(self._map)
            for p, uri in decl:
                self._map[p] = uri
            self._names, self._anames = {}, {}
        tag = self._names.get(raw)
        if tag is None:
            tag = self._names[raw] = self._qname(raw, True)
        attrib = _odict()
        for an, av in attrs:
            name = self._anames.get(an)
            if name is None:
                name = self._anames[an] = self._qname(an, False)
            if name in attrib:
                raise _Bad("duplicate attribute")
            attrib[name] = self._value(av)
        self._stack.append((raw, tag, decl))
        self._started = True
        ev = self._events
        if decl:
            if self._t_start_ns:
                for p, uri in decl:
                    self._t_start_ns(p, uri)
            if ev is not None and "start-ns" in self._want:
                for d in decl:
                    ev.append(("start-ns", d))
        e = self._t_start(tag, attrib) if self._t_start else None
        if self._ev_start:
            ev.append(("start", e))

    def _end(self):
        raw, tag, decl = self._stack.pop()
        e = self._t_end(tag) if self._t_end else None
        ev = self._events
        if self._ev_end:
            ev.append(("end", e))
        if decl:
            self._map, self._names, self._anames = self._maps.pop()
            for p, uri in reversed(decl):
                if self._t_end_ns:
                    self._t_end_ns(p)
                if ev is not None and "end-ns" in self._want:
                    ev.append(("end-ns", None))
        if not self._stack:
            self._ended = True

    def _starttag(self, b, i, n):
        # the start tag at b[i]: the index after it, or -1 if b ends first
        j = b.find(b">", i)
        if j < 0:
            return -1
        if self._ended:
            raise _Bad("junk after document element")
        s = b[i + 1:j]
        empty = s[-1:] == b"/"
        if empty:
            s = s[:-1]
        if b'"' not in s and b"'" not in s:
            # no attributes: they need quotes
            name = s.rstrip()
            if not name or name[0] in _WS or 32 in name or 9 in name or 10 in name or 61 in name:
                raise _Bad("not well-formed (invalid token)")
            self._start(name, ())
            if empty:
                self._end()
            return j + 1
        # with attributes, whose values may hold '>'
        k = i + 1
        while k < n and b[k] not in _NAME_END:
            k += 1
        name = b[i + 1:k]
        attrs = []
        while True:
            a = k
            while k < n and b[k] in _WS:
                k += 1
            if k >= n:
                return -1
            c = b[k]
            if c == 62:  # >
                k += 1
                empty = False
                break
            if c == 47:  # />
                if k + 1 >= n:
                    return -1
                if b[k + 1] != 62:
                    raise _Bad("not well-formed (invalid token)")
                k += 2
                empty = True
                break
            if k == a:
                raise _Bad("not well-formed (invalid token)")
            a = k
            while k < n and b[k] not in _NAME_END:
                k += 1
            an = b[a:k]
            while k < n and b[k] in _WS:
                k += 1
            if k >= n:
                return -1
            if b[k] != 61 or not an:  # =
                raise _Bad("not well-formed (invalid token)")
            k += 1
            while k < n and b[k] in _WS:
                k += 1
            if k >= n:
                return -1
            q = b[k]
            if q != 34 and q != 39:
                raise _Bad("not well-formed (invalid token)")
            e = b.find(b'"' if q == 34 else b"'", k + 1)
            if e < 0:
                return -1
            attrs.append((an, b[k + 1:e]))
            k = e + 1
        if not name:
            raise _Bad("not well-formed (invalid token)")
        self._start(name, attrs)
        if empty:
            self._end()
        return k

    def _doctype(self, b, i):
        # the index after <!DOCTYPE ...>, with its [internal subset]
        gt = b.find(b">", i)
        sq = b.find(b"[", i)
        if sq >= 0 and (gt < 0 or sq < gt):
            k = b.find(b"]", sq)
            while k >= 0:
                j = k + 1
                while j < len(b) and b[j] in _WS:
                    j += 1
                if j < len(b) and b[j] == 62:
                    return j + 1
                k = b.find(b"]", k + 1)
            return -1
        return gt + 1 if gt >= 0 else -1

    def _scan(self, final):
        if self._dec is None:
            dec = self._encoding(self._buf, final)
            if dec is False:
                return
            self._dec = dec
        b = self._buf
        n = len(b)
        i = 0
        dec = self._dec
        data = self._t_data
        try:
            while i < n:
                if b[i] != 60:  # text up to the next <
                    j = b.find(b"<", i)
                    if j < 0:
                        if final:
                            j = n
                        elif self._stack and n - i > 512:
                            # a long text: pass on what is whole, so that
                            # the buffer does not grow (nor fragment the heap)
                            j = n - 2  # keep what may be the start of ]]>
                            a = b.rfind(b"&", i, j)
                            if a >= 0 and b.find(b";", a, j) < 0:
                                j = a  # not a split &entity;
                            while j > i and 128 <= b[j] < 192:
                                j -= 1  # nor a split UTF-8 character
                            if j <= i:
                                break
                        else:
                            break
                    if self._stack:
                        s = dec(b[i:j])
                        if "&" in s:
                            s = self._ents(s)
                        if "]]>" in s:
                            raise _Bad("not well-formed (invalid token)")
                        if data:
                            data(s)
                    elif b[i:j].strip():
                        raise _Bad("junk after document element" if self._ended else "syntax error")
                    i = j
                    continue
                if i + 1 >= n:
                    if final:
                        raise _Bad("unclosed token")
                    break
                c = b[i + 1]
                if c == 47:  # </name>
                    j = b.find(b">", i)
                    if j < 0:
                        if final:
                            raise _Bad("unclosed token")
                        break
                    if not self._stack or self._stack[-1][0] != b[i + 2:j].rstrip():
                        raise _Bad("mismatched tag")
                    self._end()
                    i = j + 1
                elif c == 33:  # <!
                    if b.startswith(b"<!--", i):
                        j = b.find(b"-->", i + 4)
                        if j < 0:
                            if final:
                                raise _Bad("unclosed token")
                            break
                        s = dec(b[i + 4:j])
                        e = self._t_comment(s) if self._t_comment else None
                        if self._events is not None and "comment" in self._want:
                            self._events.append(("comment", e))
                        i = j + 3
                    elif b.startswith(b"<![CDATA[", i):
                        if not self._stack:
                            raise _Bad("syntax error")
                        j = b.find(b"]]>", i + 9)
                        if j < 0:
                            if final:
                                raise _Bad("unclosed CDATA section")
                            break
                        if j > i + 9 and data:
                            data(dec(b[i + 9:j]))
                        i = j + 3
                    elif b.startswith(b"<!DOCTYPE", i):
                        if self._started:
                            raise _Bad("syntax error")
                        j = self._doctype(b, i)
                        if j < 0:
                            if final:
                                raise _Bad("unclosed token")
                            break
                        d = b[i:j]
                        k = d.find(b"[")
                        d = d[:k] if k >= 0 else d
                        self._dtd = b"PUBLIC" in d or b"SYSTEM" in d
                        i = j
                    else:
                        if n - i < 9 and not final:
                            break
                        raise _Bad("syntax error")
                elif c == 63:  # <?target text?>
                    j = b.find(b"?>", i + 2)
                    if j < 0:
                        if final:
                            raise _Bad("unclosed token")
                        break
                    s = dec(b[i + 2:j])
                    t = s.split(None, 1)
                    if not t or s[0] in " \t\n":
                        raise _Bad("not well-formed (invalid token)")
                    if t[0].lower() == "xml":
                        if i + self._col or self._lines or self._started:
                            raise _Bad("XML or text declaration not at start of entity")
                    else:
                        text = t[1] if len(t) > 1 else ""
                        e = self._t_pi(t[0], text) if self._t_pi else None
                        if self._events is not None and "pi" in self._want:
                            self._events.append(("pi", e))
                    i = j + 2
                else:
                    j = self._starttag(b, i, n)
                    if j < 0:
                        if final:
                            raise _Bad("unclosed token")
                        break
                    i = j
        except _Bad as e:
            raise self._error(str(e), b, i)
        nl = b.rfind(b"\n", 0, i)
        self._col = i - nl - 1 if nl >= 0 else self._col + i
        self._lines += b.count(b"\n", 0, i)
        self._buf = b[i:] if i else b


class XMLPullParser:
    def __init__(self, events=None, *, _parser=None):
        self._queue = []
        self._parser = _parser or XMLParser(target=TreeBuilder())
        self._parser._setevents(self._queue, events or ("end",))

    def feed(self, data):
        if self._parser is None:
            raise ValueError("feed() called after end of stream")
        if data:
            try:
                self._parser.feed(data)
            except SyntaxError as exc:
                self._queue.append(exc)

    def _close_and_return_root(self):
        root = self._parser.close()
        self._parser = None
        return root

    def close(self):
        self._close_and_return_root()

    def read_events(self):
        q = self._queue
        while q:
            ev = q.pop(0)
            if isinstance(ev, Exception):
                raise ev
            yield ev

    def flush(self):
        pass


class _IterParse:
    def __init__(self, source, events, parser):
        self.root = None
        self._it = self._run(source, XMLPullParser(events, _parser=parser))

    def _run(self, source, pull):
        f = open(source, "rb") if not hasattr(source, "read") else source
        old = _gc_often()
        try:
            while True:
                yield from pull.read_events()
                data = f.read(_CHUNK)
                if not data:
                    break
                pull.feed(data)
            root = pull._close_and_return_root()
            yield from pull.read_events()
            self.root = root
        finally:
            if f is not source:
                f.close()
            _gc_restore(old)

    def __iter__(self):
        return self

    def __next__(self):
        return next(self._it)

    def close(self):
        self._it.close()


def iterparse(source, events=None, parser=None):
    """(event, element) pairs while reading source (a file name or an
    object with read()); events: "start", "end" (the default), "start-ns",
    "end-ns", "comment", "pi". An element is complete at its "end"; clear()
    it there to keep memory low."""
    return _IterParse(source, events, parser)


# --- writing ------------------------------------------------------------

_namespace_map = {
    "http://www.w3.org/XML/1998/namespace": "xml",
    "http://www.w3.org/1999/xhtml": "html",
    "http://www.w3.org/1999/02/22-rdf-syntax-ns#": "rdf",
    "http://schemas.xmlsoap.org/wsdl/": "wsdl",
    "http://www.w3.org/2001/XMLSchema": "xs",
    "http://www.w3.org/2001/XMLSchema-instance": "xsi",
    "http://purl.org/dc/elements/1.1/": "dc",
}


def register_namespace(prefix, uri):
    if prefix[:2] == "ns" and prefix[2:].isdigit():
        raise ValueError("Prefix format reserved for internal use")
    for k, v in list(_namespace_map.items()):
        if k == uri or v == prefix:
            del _namespace_map[k]
    _namespace_map[uri] = prefix


def _cannot(x):
    raise TypeError("cannot serialize %r (type %s)" % (x, type(x).__name__))


def _escape_cdata(s):
    if not isinstance(s, str):
        _cannot(s)
    if "&" in s:
        s = s.replace("&", "&amp;")
    if "<" in s:
        s = s.replace("<", "&lt;")
    if ">" in s:
        s = s.replace(">", "&gt;")
    return s


def _escape_attrib(s):
    s = _escape_cdata(s)
    if '"' in s:
        s = s.replace('"', "&quot;")
    if "\r" in s:
        s = s.replace("\r", "&#13;")
    if "\n" in s:
        s = s.replace("\n", "&#10;")
    if "\t" in s:
        s = s.replace("\t", "&#09;")
    return s


def _qnames(root, default_namespace):
    # {tag or attribute name: as written}, {uri: prefix} to declare at the root
    qnames = {None: None}
    ns = {}
    if default_namespace:
        ns[default_namespace] = ""

    def add(q):
        if not isinstance(q, str):
            _cannot(q)
        if q[:1] == "{":
            uri, local = q[1:].rsplit("}", 1)
            prefix = ns.get(uri)
            if prefix is None:
                prefix = _namespace_map.get(uri)
                if prefix is None:
                    prefix = "ns%d" % len(ns)
                if prefix != "xml":
                    ns[uri] = prefix
            qnames[q] = prefix + ":" + local if prefix else local
        else:
            if default_namespace:
                raise ValueError("cannot use non-qualified names with default_namespace option")
            qnames[q] = q

    for e in root.iter():
        tag = e.tag
        if isinstance(tag, str):
            if tag not in qnames:
                add(tag)
        elif tag is not None and tag is not Comment and tag is not ProcessingInstruction:
            _cannot(tag)
        for k in e.attrib:
            if k not in qnames:
                add(k)
    return qnames, ns


def _write_xml(w, e, qnames, ns, short):
    tag = e.tag
    if tag is Comment:
        w("<!--%s-->" % e.text)
    elif tag is ProcessingInstruction:
        w("<?%s?>" % e.text)
    else:
        tag = qnames[tag]
        if tag is None:
            if e.text:
                w(_escape_cdata(e.text))
            for c in e:
                _write_xml(w, c, qnames, None, short)
        else:
            w("<" + tag)
            if ns:
                for uri, prefix in sorted(ns.items(), key=lambda x: x[1]):
                    w(' xmlns%s="%s"' % (":" + prefix if prefix else "", _escape_attrib(uri)))
            for k, v in e.attrib.items():
                w(' %s="%s"' % (qnames[k], _escape_attrib(v)))
            if e.text or len(e) or not short:
                w(">")
                if e.text:
                    w(_escape_cdata(e.text))
                for c in e:
                    _write_xml(w, c, qnames, None, short)
                w("</" + tag + ">")
            else:
                w(" />")
    if e.tail:
        w(_escape_cdata(e.tail))


def _ascii(s):
    if len(s.encode()) == len(s):
        return s
    return "".join([c if ord(c) < 128 else "&#%d;" % ord(c) for c in s])


def _serialize(root, encoding, xml_declaration, default_namespace, method, short):
    method = method or "xml"
    if method not in ("xml", "text"):
        raise ValueError("unknown method %r" % method)
    encoding = encoding or "us-ascii"
    enc = encoding.lower()
    if enc not in ("unicode", "utf-8", "utf8", "us-ascii", "ascii"):
        raise LookupError("unknown encoding: %s" % encoding)
    out = []
    w = out.append
    if method == "xml" and (xml_declaration or xml_declaration is None and
                            enc not in ("unicode", "utf-8", "us-ascii")):
        w("<?xml version='1.0' encoding='%s'?>\n" % ("utf-8" if enc == "unicode" else encoding))
    if method == "text":
        w("".join(root.itertext()))
        if root.tail:
            w(root.tail)
    else:
        qnames, ns = _qnames(root, default_namespace)
        _write_xml(w, root, qnames, ns, short)
    if enc in ("us-ascii", "ascii"):
        out = [_ascii(p) for p in out]  # piece by piece: less memory
    s = "".join(out)
    return s if enc == "unicode" else s.encode()


def tostring(element, encoding=None, method=None, *, xml_declaration=None,
             default_namespace=None, short_empty_elements=True):
    return _serialize(element, encoding, xml_declaration, default_namespace, method,
                      short_empty_elements)


def tostringlist(element, encoding=None, method=None, *, xml_declaration=None,
                 default_namespace=None, short_empty_elements=True):
    return [tostring(element, encoding, method, xml_declaration=xml_declaration,
                     default_namespace=default_namespace,
                     short_empty_elements=short_empty_elements)]


def dump(elem):
    if not isinstance(elem, ElementTree):
        elem = ElementTree(elem)
    s = tostring(elem.getroot(), encoding="unicode")
    print(s, end="" if s.endswith("\n") else "\n")


def indent(tree, space="  ", level=0):
    """Whitespace in text and tails so that tostring() shows one element
    per line, indented by depth."""
    if isinstance(tree, ElementTree):
        tree = tree.getroot()
    if level < 0:
        raise ValueError("Initial indentation level must be >= 0, got %d" % level)

    def walk(e, depth):
        if not len(e):
            return
        inner = "\n" + space * (depth + 1)
        if not e.text or not e.text.strip():
            e.text = inner
        for c in e:
            walk(c, depth + 1)
            if not c.tail or not c.tail.strip():
                c.tail = inner
        if not c.tail.strip():
            c.tail = "\n" + space * depth

    walk(tree, level)
