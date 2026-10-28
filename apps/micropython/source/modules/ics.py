# SPDX-License-Identifier: MIT
"""Calendars in iCalendar format (.ics, RFC 5545): the events, with their
repeats worked out, as times the badge's clock understands.

    import ics, time
    cal = ics.get("https://example.org/calendar.ics")   # or webcal://
    cal = ics.load("/mnt/sd/badge/appdata/micropython/my.ics")
    now = time.time()
    for e in cal.between(now, now + 7 * 86400):
        print(time.localtime(e.start), e.summary)

Times are seconds since 1970 (UTC), as time.time(); time.localtime()
shows them in the badge's time zone. An all-day event starts at midnight
of its day on the badge's clock (e.all_day). Each event has uid, summary,
location, description, status, url, start, end, all_day, rrule (the rule
as text, or None) and recurrence_id.

Time zones: UTC, the calendar's own VTIMEZONE definitions, then the
badge's zone list (/usr/share/badge/zones); a time without a zone is the
badge's own. Repeats (RRULE) with FREQ DAILY, WEEKLY, MONTHLY or YEARLY,
INTERVAL, COUNT, UNTIL, BYDAY, BYMONTHDAY, BYMONTH, BYSETPOS and WKST,
plus RDATE, EXDATE and changed or cancelled single instances
(RECURRENCE-ID). DTSTART is the first instance, also when the rule would
not give it (RFC 5545 leaves that open). A rule with anything else
(BYYEARDAY, BYWEEKNO, HOURLY, ...) gives only its first instance.
"""
import os

ZONES = "/usr/share/badge/zones"  # zone <TAB> POSIX TZ rule
LOCAL_TZ = None  # the badge's zone as a POSIX rule; None: $TZ, else /etc/TZ
_WD = ("MO", "TU", "WE", "TH", "FR", "SA", "SU")

# --- days ---------------------------------------------------------------
# A day is a number: days since 1970-01-01 (proleptic Gregorian).


def days(y, m, d):
    y -= m <= 2
    era = y // 400
    yoe = y - era * 400
    doy = (153 * (m + (-3 if m > 2 else 9)) + 2) // 5 + d - 1
    return era * 146097 + yoe * 365 + yoe // 4 - yoe // 100 + doy - 719468


def civil(n):
    """(year, month, day) of day number n."""
    n += 719468
    era = n // 146097
    doe = n - era * 146097
    yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    mp = (5 * doy + 2) // 153
    d = doy - (153 * mp + 2) // 5 + 1
    m = mp + 3 if mp < 10 else mp - 9
    return yoe + era * 400 + (m <= 2), m, d


def _weekday(n):  # Monday 0
    return (n + 3) % 7


def _mdays(y, m):
    if m == 2:
        return 29 if y % 4 == 0 and (y % 100 != 0 or y % 400 == 0) else 28
    return 30 if m in (4, 6, 9, 11) else 31


# --- time zones ---------------------------------------------------------
# offset(utc): seconds east of UTC at that moment; utc(local): the moment
# a wall-clock time (day * 86400 + seconds, as if UTC) is. A time that
# occurs twice is the first; one skipped by a jump forward is taken in
# the offset before the jump (as Python's zoneinfo, fold=0).


class _Zone:
    def utc(self, local):
        before = self.offset(local - 86400)
        after = self.offset(local + 86400)
        best = None
        for o in (before, after):
            if self.offset(local - o) == o and (best is None or local - o < best):
                best = local - o
        return local - before if best is None else best


class _Fixed(_Zone):
    def __init__(self, off):
        self.off = off

    def offset(self, u):
        return self.off


UTC = _Fixed(0)


class _Posix(_Zone):
    """A POSIX TZ rule, such as CET-1CEST,M3.5.0,M10.5.0/3."""

    def __init__(self, std, dst, start, end):
        self.std, self.dst, self.start, self.end = std, dst, start, end
        self._years = {}

    def offset(self, u):
        if self.dst is None:
            return self.std
        y = civil(u // 86400)[0]
        t = self._years.get(y)
        if t is None:
            s = _rule_day(self.start, y) * 86400 + self.start[-1] - self.std
            e = _rule_day(self.end, y) * 86400 + self.end[-1] - self.dst
            t = self._years[y] = (s, e)
        s, e = t
        if s < e:
            return self.dst if s <= u < e else self.std
        return self.std if e <= u < s else self.dst


def _rule_day(r, y):
    if r[0] == "M":
        m, w, d = r[1], r[2], r[3]
        first = days(y, m, 1)
        n = first + (d - (_weekday(first) + 1) % 7) % 7 + 7 * (w - 1)
        while n >= first + _mdays(y, m):
            n -= 7
        return n
    if r[0] == "J":  # 1..365, February 29 not counted
        return days(y, 1, 1) + r[1] - 1 + (r[1] >= 60 and _mdays(y, 2) == 29)
    return days(y, 1, 1) + r[1]


def posix(s):
    """A zone from a POSIX TZ rule; ValueError when it is not one."""
    p = [0]

    def name():
        i = p[0]
        if s[i:i + 1] == "<":
            j = s.index(">", i)
            p[0] = j + 1
            return
        while i < len(s) and s[i].isalpha():
            i += 1
        if i - p[0] < 3:
            raise ValueError(s)
        p[0] = i

    def hms():
        i, sign = p[0], 1
        if s[i:i + 1] in ("+", "-"):
            sign = -1 if s[i] == "-" else 1
            i += 1
        j = i
        while j < len(s) and (s[j].isdigit() or s[j] == ":"):
            j += 1
        if j == i:
            raise ValueError(s)
        f = [int(x) for x in s[i:j].split(":")] + [0, 0]
        p[0] = j
        return sign * (f[0] * 3600 + f[1] * 60 + f[2])

    def rule():
        i = p[0]
        j = i
        while j < len(s) and s[j] not in ",/":
            j += 1
        r = s[i:j]
        if r[0] == "M":
            r = ["M"] + [int(x) for x in r[1:].split(".")]
        elif r[0] == "J":
            r = ["J", int(r[1:])]
        else:
            r = ["", int(r)]
        p[0] = j
        t = 7200
        if s[j:j + 1] == "/":
            p[0] = j + 1
            t = hms()
        return r + [t]

    name()
    std = -hms()
    if p[0] == len(s):
        return _Fixed(std)
    name()
    dst = std + 3600
    if p[0] < len(s) and s[p[0]] != ",":
        dst = -hms()
    if s[p[0]:p[0] + 1] != ",":  # no rule: as glibc, the US one
        start, end = ["M", 3, 2, 0, 7200], ["M", 11, 1, 0, 7200]
    else:
        p[0] += 1
        start = rule()
        if s[p[0]:p[0] + 1] != ",":
            raise ValueError(s)
        p[0] += 1
        end = rule()
    return _Posix(std, dst, start, end)


class _VZone(_Zone):
    """A VTIMEZONE: onsets of standard and daylight time, each with the
    offset it starts; offset() is that of the latest onset."""

    def __init__(self, obs):
        self.obs = obs  # [dn, tod, from, to, rule, rdates]
        self._years = {}
        self.first = min(o[0] for o in obs) if obs else 0
        early = min(obs, key=lambda o: o[0]) if obs else None
        self.base = early[2] if early else 0

    def _year(self, y):
        t = self._years.get(y)
        if t is None:
            lo, hi = days(y, 1, 1), days(y, 12, 31)
            t = []
            for dn, tod, frm, to, rule, rdates in self.obs:
                at = lambda n, tod=tod, frm=frm: n * 86400 + tod - frm
                if rule is not None:
                    for n, u in _starts(rule, dn, at, lo, hi):
                        if n >= lo:
                            t.append((u, to))
                elif lo <= dn <= hi:
                    t.append((at(dn), to))
                for n, rtod in rdates:
                    if lo <= n <= hi:
                        t.append((n * 86400 + rtod - frm, to))
            t.sort()
            self._years[y] = t
        return t

    def offset(self, u):
        y = civil(u // 86400)[0]
        while y >= civil(self.first)[0]:
            best = None
            for t, to in self._year(y):
                if t <= u:
                    best = to
            if best is not None:
                return best
            y -= 1
        return self.base


_zone_list = None


def _local():
    global LOCAL_TZ
    if LOCAL_TZ is None:
        LOCAL_TZ = os.getenv("TZ") or ""
        if not LOCAL_TZ:
            try:
                with open("/etc/TZ") as f:
                    LOCAL_TZ = f.readline().strip()
            except OSError:
                pass
    try:
        return posix(LOCAL_TZ or "UTC0")
    except (ValueError, IndexError):
        return UTC


def zone(name):
    """A zone by its name in the badge's zone list, or None."""
    global _zone_list
    if name in ("UTC", "GMT", "Etc/UTC", "Etc/GMT", "Z"):
        return UTC
    if _zone_list is None:
        _zone_list = {}
        try:
            with open(ZONES) as f:
                for line in f:
                    if line[0] != "#" and "\t" in line:
                        k, v = line.strip().split("\t", 1)
                        _zone_list[k] = v
        except OSError:
            pass
    rule = _zone_list.get(name)
    if rule is None:  # "/mozilla.org/20050126_1/Europe/Amsterdam" and such
        for k in _zone_list:
            if name.endswith("/" + k):
                rule = _zone_list[k]
                break
    return posix(rule) if rule else None


# --- recurrence ---------------------------------------------------------


def _rule(text):
    """RRULE text as a dict; "x" set when it has parts not supported."""
    r = {"INTERVAL": 1, "WKST": 0}
    for part in text.split(";"):
        k, _, v = part.partition("=")
        k = k.upper()
        v = v.upper()
        if k == "FREQ":
            r[k] = v
            if v not in ("DAILY", "WEEKLY", "MONTHLY", "YEARLY"):
                r["x"] = 1
        elif k in ("INTERVAL", "COUNT"):
            r[k] = int(v)
        elif k == "UNTIL":
            r[k] = v
        elif k == "WKST":
            r[k] = _WD.index(v)
        elif k == "BYDAY":
            r[k] = [(int(d[:-2]) if d[:-2] else 0, _WD.index(d[-2:])) for d in v.split(",")]
        elif k in ("BYMONTHDAY", "BYMONTH", "BYSETPOS"):
            r[k] = [int(x) for x in v.split(",")]
        elif k in ("BYHOUR", "BYMINUTE", "BYSECOND"):
            if "," in v:  # one value only sets the time, as DTSTART does
                r["x"] = 1
        elif k:
            r["x"] = 1
    if "FREQ" not in r:
        r["x"] = 1
    return r


def _byday(a, b, byday):
    """Days a..b (day numbers) that BYDAY picks, ordinals counted in a..b."""
    out = []
    for n, wd in byday:
        first = a + (wd - _weekday(a)) % 7
        if n == 0:
            out.extend(range(first, b + 1, 7))
        elif n > 0:
            x = first + 7 * (n - 1)
            if x <= b:
                out.append(x)
        else:
            x = b - (_weekday(b) - wd) % 7 + 7 * (n + 1)
            if x >= a:
                out.append(x)
    return out


def _in_month(y, m, bymd, byday, d0):
    a = days(y, m, 1)
    n = _mdays(y, m)
    if bymd is None and byday is None:
        return [a + d0 - 1] if d0 <= n else []
    s = None
    if bymd is not None:
        s = []
        for x in bymd:
            x = x if x > 0 else n + x + 1
            if 1 <= x <= n:
                s.append(a + x - 1)
    if byday is not None:
        b = _byday(a, a + n - 1, byday)
        s = b if s is None else [x for x in s if x in b]
    return s


def _rrule(r, dn0, lo, hi):
    """Day numbers the rule gives from dn0 (DTSTART's day) up to hi, in
    order; without COUNT, periods before lo are skipped."""
    f, iv = r["FREQ"], r["INTERVAL"]
    byday, bymd, bym, pos = r.get("BYDAY"), r.get("BYMONTHDAY"), r.get("BYMONTH"), r.get("BYSETPOS")
    y0, m0, d0 = civil(dn0)
    ws = dn0 - (_weekday(dn0) - r["WKST"]) % 7  # the start of DTSTART's week
    k = 0
    if "COUNT" not in r and lo > dn0:
        if f == "DAILY":
            k = (lo - dn0) // iv
        elif f == "WEEKLY":
            k = (lo - ws) // (7 * iv)
        elif f == "MONTHLY":
            yl, ml, _ = civil(lo)
            k = (yl * 12 + ml - y0 * 12 - m0) // iv
        else:
            k = (civil(lo)[0] - y0) // iv
    while True:
        if f == "DAILY":
            p = dn0 + k * iv
            if p > hi:
                return
            y, m, d = civil(p)
            c = [p]
            if (bym and m not in bym) or (byday and _weekday(p) not in [w for _, w in byday]) or \
                    (bymd and d not in bymd and d - _mdays(y, m) - 1 not in bymd):
                c = []
        elif f == "WEEKLY":
            p = ws + k * 7 * iv
            if p > hi:
                return
            wds = [w for _, w in byday] if byday else [_weekday(dn0)]
            c = [p + (w - r["WKST"]) % 7 for w in wds]
            if bym:
                c = [x for x in c if civil(x)[1] in bym]
        elif f == "MONTHLY":
            y, m = divmod(y0 * 12 + m0 - 1 + k * iv, 12)
            m += 1
            if days(y, m, 1) > hi:
                return
            c = [] if bym and m not in bym else _in_month(y, m, bymd, byday, d0)
        else:  # YEARLY
            y = y0 + k * iv
            if days(y, 1, 1) > hi:
                return
            if bym or bymd is not None:
                c = []
                for m in sorted(bym) if bym else range(1, 13):
                    c.extend(_in_month(y, m, bymd, byday, d0))
            elif byday is not None:
                c = _byday(days(y, 1, 1), days(y, 12, 31), byday)
            else:
                c = [days(y, m0, d0)] if d0 <= _mdays(y, m0) else []
        k += 1
        c = sorted(set(c))
        if pos:
            n = len(c)
            c = sorted(set(c[i - 1 if i > 0 else n + i] for i in pos if i and abs(i) <= n))
        for x in c:
            if x > hi:
                return
            if x >= dn0:
                yield x


def _starts(r, dn0, at, lo, hi):
    """(day, moment) of each instance up to day hi, DTSTART first, with
    COUNT and UNTIL (r["_until"], a moment); at(day) gives the moment."""
    n = 0
    if dn0 <= hi:
        n = 1
        yield dn0, at(dn0)
    if "x" in r:
        return
    until, count = r.get("_until"), r.get("COUNT")
    for d in _rrule(r, dn0, lo, hi):
        if d == dn0:
            continue
        n += 1
        if count is not None and n > count:
            return
        u = at(d)
        if until is not None and u > until:
            return
        yield d, u


# --- values -------------------------------------------------------------


def _dt(v):
    """20261028 or 20261028T120000[Z]: (day, seconds into it, is a date)."""
    dn = days(int(v[0:4]), int(v[4:6]), int(v[6:8]))
    if len(v) < 15:
        return dn, 0, True
    return dn, int(v[9:11]) * 3600 + int(v[11:13]) * 60 + int(v[13:15]), False


def _duration(v):
    sign = -1 if v[:1] == "-" else 1
    n, total, t = 0, 0, False
    for ch in v.lstrip("+-"):
        if ch.isdigit():
            n = n * 10 + int(ch)
        else:
            total += n * {"W": 604800, "D": 86400, "H": 3600, "M": 60 if t else 0, "S": 1}.get(ch, 0)
            n = 0
            if ch == "T":
                t = True
    return sign * total


def _unescape(s):
    if "\\" not in s:
        return s
    out, i = [], 0
    while i < len(s):
        ch = s[i]
        if ch == "\\" and i + 1 < len(s):
            i += 1
            ch = s[i]
            if ch in "nN":
                ch = "\n"
        out.append(ch)
        i += 1
    return "".join(out)


def _text(b):
    try:
        return b.decode()
    except UnicodeError:  # not UTF-8: Latin-1
        return "".join(chr(c) for c in b)


def _split(line):
    """name, {param: value}, value."""
    i = line.find(":")
    if i < 0:
        return line.upper(), {}, ""
    head = line[:i]
    if '"' in head:  # a quoted parameter may hold a colon
        q = False
        for i, ch in enumerate(line):
            if ch == '"':
                q = not q
            elif ch == ":" and not q:
                break
        head = line[:i]
    parts = head.split(";")
    params = {}
    for p in parts[1:]:
        k, _, v = p.partition("=")
        params[k.upper()] = v.strip('"')
    return parts[0].upper(), params, line[i + 1:]


def _lines(read):
    """Logical lines: a line that starts with a space or tab continues the
    one before (folding), joined before decoding."""
    buf, line = b"", None
    while True:
        chunk = read(1024)
        if chunk:
            buf += chunk
        parts = buf.split(b"\n")
        buf = parts.pop() if chunk else b""
        for p in parts:
            if p[-1:] == b"\r":
                p = p[:-1]
            if p[:1] in (b" ", b"\t") and line is not None:
                line += p[1:]
            else:
                if line:
                    yield _text(line)
                line = p
        if not chunk:
            break
    if line:
        yield _text(line)


# --- the calendar -------------------------------------------------------


class Event:
    def __init__(self):
        self.uid = self.summary = self.location = self.description = None
        self.status = self.url = self.start = self.end = self.rrule = None
        self.recurrence_id = None
        self.all_day = False

    def __repr__(self):
        y, m, d = civil(self.start // 86400)
        return "<Event %04d-%02d-%02d %r>" % (y, m, d, self.summary)

    def _copy(self, start, end):
        return _Instance(self, start, end)


class _Instance:
    """One time of a repeating event: its own start and end, the rest the
    event's (a small object, so a long list of repeats fits in memory)."""

    def __init__(self, event, start, end):
        self.event, self.start, self.end = event, start, end

    def __getattr__(self, name):
        return getattr(self.event, name)

    __repr__ = Event.__repr__


class Calendar:
    def __init__(self):
        self.name = None  # X-WR-CALNAME
        self.events = []  # as in the file: one per VEVENT, repeats not worked out
        self._zones = {}

    def _zone(self, params, v):
        if v.endswith("Z"):
            return UTC
        name = params.get("TZID")
        if not name:
            return self._local
        z = self._zones.get(name)
        if z is None:
            z = zone(name) or self._local
            self._zones[name] = z
        return z

    def _moment(self, v, params, z=None):
        dn, tod, date = _dt(v)
        return (z or self._zone(params, v)).utc(dn * 86400 + tod)

    def _resolve(self, e, p):
        v, pr = p["DTSTART"]
        z = self._zone(pr, v)
        dn, tod, e.all_day = _dt(v)
        if e.all_day:
            z = self._local
        e._z, e._dn, e._tod = z, dn, tod
        e.start = z.utc(dn * 86400 + tod)
        e._days = e._dur = 0
        if "DTEND" in p:
            ev, epr = p["DTEND"]
            if e.all_day:
                e._days = _dt(ev)[0] - dn
            else:
                e._dur = self._moment(ev, epr) - e.start
        elif "DURATION" in p:
            d = _duration(p["DURATION"][0])
            if e.all_day:
                e._days = d // 86400
            else:
                e._dur = d
        elif e.all_day:
            e._days = 1
        e.end = self._end(e, dn, e.start)
        e._rule = None
        if "RRULE" in p:
            e.rrule = p["RRULE"][0]
            r = e._rule = _rule(e.rrule)
            u = r.get("UNTIL")
            if u:
                udn, utod, udate = _dt(u)
                if udate and not e.all_day:  # the whole day
                    r["_until"] = z.utc((udn + 1) * 86400) - 1
                else:
                    r["_until"] = (UTC if u.endswith("Z") else z).utc(udn * 86400 + utod)
        e._rdates = []
        for v, pr in p.get("RDATE", ()):
            for x in v.split(","):
                x = x.split("/")[0]
                rdn, rtod, rdate = _dt(x)
                rz = self._local if rdate else self._zone(pr, x)
                e._rdates.append((rdn, rz.utc(rdn * 86400 + rtod)))
        e._ex = set()
        for v, pr in p.get("EXDATE", ()):
            for x in v.split(","):
                xdn, xtod, xdate = _dt(x)
                e._ex.add((self._local if xdate else self._zone(pr, x)).utc(xdn * 86400 + xtod))
        if "RECURRENCE-ID" in p:
            v, pr = p["RECURRENCE-ID"]
            rdn, rtod, rdate = _dt(v)
            e.recurrence_id = (self._local if rdate else self._zone(pr, v)).utc(rdn * 86400 + rtod)

    def _end(self, e, dn, start):
        return e._z.utc((dn + e._days) * 86400) if e.all_day else start + e._dur

    def between(self, start, end):
        """The events (each repeat its own) that overlap start..end
        (seconds since 1970, UTC), by start."""
        moved = {}
        for e in self.events:
            if e.recurrence_id is not None:
                moved.setdefault(e.uid, set()).add(e.recurrence_id)
        out = []
        for e in self.events:
            if e.status == "CANCELLED":
                continue
            if e._rule is None and not e._rdates:
                if e.end > start and e.start < end or e.start == start:
                    out.append(e)
                continue
            skip = e._ex | moved.get(e.uid, set())
            span = e._days * 86400 + max(e._dur, 0) + 2 * 86400
            lo, hi = (start - span) // 86400, end // 86400 + 1
            at = lambda n, e=e: e._z.utc(n * 86400 + e._tod)
            got = [] if e._rule is None else list(_starts(e._rule, e._dn, at, lo, hi))
            if e._rule is None:
                got.append((e._dn, e.start))
            got.extend(e._rdates)
            seen = set()
            for dn, s in got:
                if s in skip or s in seen:
                    continue
                seen.add(s)
                t = self._end(e, dn, s)
                if t > start and s < end or s == start:
                    out.append(e._copy(s, t))
        out.sort(key=lambda e: (e.start, e.end, e._n))  # MicroPython's sort is not stable
        return out


def parse(src, since=None):
    """A Calendar from .ics text (str or bytes) or a stream with read().
    since (seconds since 1970): events without repeats that ended more
    than two days before it are left out, which saves memory."""
    if isinstance(src, str):
        src = src.encode()
    if isinstance(src, bytes):
        data = [src]
        read = lambda n: data.pop() if data else b""
    else:
        read = src.read
    cut = None
    if since is not None:
        y, m, d = civil(since // 86400 - 2)
        cut = "%04d%02d%02d" % (y, m, d)
    cal = Calendar()
    cal._local = _local()
    stack, events, zones = [], [], []
    e = p = ob = None
    for line in _lines(read):
        name, params, value = _split(line)
        if name == "BEGIN":
            c = value.upper()
            stack.append(c)
            if c == "VEVENT" and len(stack) <= 2:
                e, p = Event(), {}
            elif c == "VTIMEZONE":
                zones.append(["", []])
            elif c in ("STANDARD", "DAYLIGHT") and zones:
                ob = {}
            continue
        if name == "END":
            c = stack.pop() if stack else ""
            if c == "VEVENT" and e is not None:
                if "DTSTART" in p:
                    old = cut and "RRULE" not in p and "RDATE" not in p and \
                        p.get("DTEND", p["DTSTART"])[0][:8] < cut
                    if cut and "RRULE" in p and "RDATE" not in p:
                        u = _rule(p["RRULE"][0]).get("UNTIL")
                        old = u is not None and u[:8] < cut
                    if not old:
                        events.append((e, p))
                e = p = None
            elif c in ("STANDARD", "DAYLIGHT") and ob is not None:
                zones[-1][1].append(ob)
                ob = None
            continue
        top = stack[-1] if stack else ""
        if top == "VEVENT" and e is not None:
            if name in ("SUMMARY", "LOCATION", "DESCRIPTION"):
                setattr(e, name.lower(), _unescape(value))
            elif name in ("UID", "URL"):
                setattr(e, name.lower(), value)
            elif name == "STATUS":
                e.status = value.upper()
            elif name in ("RDATE", "EXDATE"):
                p.setdefault(name, []).append((value, params))
            elif name in ("DTSTART", "DTEND", "DURATION", "RRULE", "RECURRENCE-ID"):
                p[name] = (value, params)
        elif top == "VTIMEZONE" and name == "TZID":
            zones[-1][0] = value
        elif ob is not None and name in ("DTSTART", "TZOFFSETFROM", "TZOFFSETTO", "RRULE"):
            ob[name] = value
        elif ob is not None and name == "RDATE":
            ob.setdefault("RDATE", []).append(value)
        elif top == "VCALENDAR" and name == "X-WR-CALNAME":
            cal.name = _unescape(value)
    for tzid, obs in zones:
        out = []
        for o in obs:
            if "DTSTART" not in o:
                continue
            dn, tod, _ = _dt(o["DTSTART"])
            frm, to = _offset(o.get("TZOFFSETFROM", "+0000")), _offset(o.get("TZOFFSETTO", "+0000"))
            rule = None
            if "RRULE" in o:
                rule = _rule(o["RRULE"])
                if rule.get("UNTIL"):
                    udn, utod, _ = _dt(rule["UNTIL"])
                    rule["_until"] = udn * 86400 + utod - (0 if rule["UNTIL"].endswith("Z") else frm)
            rd = []
            for v in o.get("RDATE", ()):
                for x in v.split(","):
                    rdn, rtod, _ = _dt(x)
                    rd.append((rdn, rtod))
            out.append([dn, tod, frm, to, rule, rd])
        if tzid and out:
            cal._zones[tzid] = _VZone(out)
    for n, (e, p) in enumerate(events):
        e._n = n
        cal._resolve(e, p)
        cal.events.append(e)
    return cal


def _offset(v):
    sign = -1 if v[0] == "-" else 1
    v = v.lstrip("+-")
    return sign * (int(v[0:2]) * 3600 + int(v[2:4]) * 60 + (int(v[4:6]) if len(v) >= 6 else 0))


def load(path, since=None):
    """A Calendar from an .ics file."""
    with open(path, "rb") as f:
        return parse(f, since)


def get(url, since=None):
    """A Calendar from an .ics link (http, https or webcal)."""
    import requests

    if url.startswith("webcal://"):
        url = "https://" + url[9:]
    r = requests.get(url)
    try:
        if r.status_code != 200:
            raise OSError("HTTP %d" % r.status_code)
        return parse(r.raw, since)
    finally:
        r.close()
