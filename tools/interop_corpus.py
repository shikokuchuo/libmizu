#!/usr/bin/env python3
"""tools/interop_corpus.py — the reference implementation of the 'I' wire
format (DESIGN.md's Interchange codec section is the authority) and the
golden-corpus generator.

Reads tests/interop/cases.txt (id | kind | langs | value | note) and
writes tests/interop/corpus.txt (id | hex). Independent of the C cursor
by construction: a merge needs agreement between this generator, the
core cursor and emitters, and each binding's walk and builder.

Checks performed here:
- rt rows: the value encodes, and decoding the encoding returns the
  value (both directions exact — rt rows never name a shifted home).
- dec rows (wire => home): the wire side encodes and decodes cleanly;
  the home side only parses (its equality is the binding's assertion).
- enc rows: the value encodes; a tuple decodes back as a list (the
  documented shift) — every other value round-trips.
- read-err rows: a "cursor:" note's authored hex fails this reader's
  scan; a "builder:" note's value encodes and scans cleanly (the
  rejection itself is the binding builder's assertion).
- write-decline rows: no corpus entry (a decline writes nothing). Rows
  this encoder can judge (duplicate or non-str dict keys, a lone
  surrogate, an int past int64) must be refused; the rest parse only.

Deterministic: libmizu CI regenerates and asserts a clean diff.
Stdlib only. Usage: tools/interop_corpus.py [--check]
"""

import re
import struct
import sys

VERSION = 0x01
DEPTH_MAX = 64
NA_REAL = 0x7FF00000000007A2   # R's actual NA_real_ bits
INT_MIN = -2**31
INT64_MIN = -2**63
INT64_MAX = 2**63 - 1

VEC_TAGS = {"lglv": 0x06, "intv": 0x07, "realv": 0x08, "cplxv": 0x09,
            "rawv": 0x0a, "i64v": 0x0e}
TASK_ARITY = {0: 3, 1: 3, 2: 3}


class Decline(Exception):
    """The writer's refusal: a value the format cannot carry."""


class IxError(Exception):
    """The reader's refusal: a malformed or unsupported stream."""


# --- the notation ------------------------------------------------------------

class _Parser:
    def __init__(self, s):
        self.s = s
        self.i = 0

    def ws(self):
        while self.i < len(self.s) and self.s[self.i] in " \t":
            self.i += 1

    def peek(self):
        self.ws()
        return self.s[self.i] if self.i < len(self.s) else ""

    def expect(self, ch):
        self.ws()
        if self.peek() != ch:
            raise ValueError("expected %r at %d of %r" % (ch, self.i, self.s))
        self.i += 1

    def token(self, stop):
        self.ws()
        j = self.i
        while self.i < len(self.s) and self.s[self.i] not in stop:
            self.i += 1
        return self.s[j:self.i].strip()

    def string(self):
        self.expect('"')
        out = []
        while True:
            if self.i >= len(self.s):
                raise ValueError("unterminated string in %r" % self.s)
            c = self.s[self.i]
            self.i += 1
            if c == '"':
                return "".join(out)
            if c == "\\":
                e = self.s[self.i]
                self.i += 1
                if e == "n":
                    out.append("\n")
                elif e == "t":
                    out.append("\t")
                elif e == "r":
                    out.append("\r")
                elif e in "\\\"":
                    out.append(e)
                elif e == "u":
                    out.append(chr(int(self.s[self.i:self.i + 4], 16)))
                    self.i += 4
                else:
                    raise ValueError("bad escape \\%s" % e)
            else:
                out.append(c)

    def real_atom(self, stop):
        tok = self.token(stop)
        if tok == "na":
            return NA_REAL
        if tok == "nan":
            return 0x7FF8000000000000
        if tok == "inf":
            return 0x7FF0000000000000
        if tok == "-inf":
            return 0xFFF0000000000000
        if tok.startswith("bits:"):
            return int(tok[5:], 16)
        return struct.unpack("<Q", struct.pack("<d", float(tok)))[0]

    def int_atom(self, stop):
        tok = self.token(stop)
        if tok == "na":
            return None
        return int(tok)

    def value(self):
        self.ws()
        for name in ("lglv", "intv", "realv", "cplxv", "rawv", "strv",
                     "i64v", "list", "tuple", "dict", "attr", "lgl", "int",
                     "real", "cplx", "str", "bytes", "err", "task", "nil"):
            if self.s.startswith(name, self.i):
                self.i += len(name)
                break
        else:
            raise ValueError("bad value at %d of %r" % (self.i, self.s))
        if name == "nil":
            return ("nil",)
        if name in VEC_TAGS or name == "strv":
            return self.vec(name)
        if name in ("list", "tuple"):
            return self.seq(name)
        if name in ("dict", "attr"):
            return self.mapping(name)
        if name == "err":
            return self.err()
        if name == "task":
            return self.task()
        self.expect("(")
        if name == "lgl":
            tok = self.token(")")
            v = {"0": 0, "1": 1, "na": 2}.get(tok)
            if v is None:
                raise ValueError("bad lgl %r" % tok)
            out = ("lgl", v)
        elif name == "int":
            out = ("int", self.int_atom(")"))
        elif name == "real":
            out = ("real", self.real_atom(")"))
        elif name == "cplx":
            re_bits = self.real_atom(",")
            self.expect(",")
            im_bits = self.real_atom(")")
            out = ("cplx", re_bits, im_bits)
        elif name == "str":
            if self.peek() == '"':
                out = ("str", self.string())
            else:
                if self.token(")") != "na":
                    raise ValueError("bad str at %d" % self.i)
                out = ("str", None)
        elif name == "bytes":
            out = ("bytes", bytes.fromhex(self.token(")")))
        else:
            raise ValueError("unhandled %s" % name)
        self.expect(")")
        return out

    def vec(self, name):
        self.expect("[")
        elts = []
        while self.peek() != "]":
            if name == "strv":
                if self.peek() == '"':
                    elts.append(self.string())
                else:
                    if self.token(",]") != "na":
                        raise ValueError("bad strv element")
                    elts.append(None)
            elif name == "cplxv":
                self.ws()
                if not self.s.startswith("cplx(", self.i):
                    raise ValueError("bad cplxv element")
                self.i += 5
                re_bits = self.real_atom(",")
                self.expect(",")
                im_bits = self.real_atom(")")
                self.expect(")")
                elts.append((re_bits, im_bits))
            elif name == "rawv":
                elts.append(int(self.token(",]"), 16))
            elif name == "realv":
                elts.append(self.real_atom(",]"))
            elif name == "lglv":
                tok = self.token(",]")
                if tok not in ("0", "1", "na"):
                    raise ValueError("bad lglv element %r" % tok)
                elts.append(2 if tok == "na" else int(tok))
            else:
                elts.append(self.int_atom(",]"))
            self.ws()
            if self.peek() == ",":
                self.i += 1
        self.expect("]")
        return ("strv", elts) if name == "strv" else ("vec", name, elts)

    def seq(self, name):
        self.expect("(")
        items = []
        while self.peek() != ")":
            items.append(self.value())
            self.ws()
            if self.peek() == ",":
                self.i += 1
        self.expect(")")
        return (name, items)

    def key(self):
        if self.peek() == '"':
            return self.string()
        tok = self.token("=")
        if re.fullmatch(r"-?[0-9]+", tok):
            return ("nonstr", int(tok))
        return tok

    def mapping(self, name):
        self.expect("(")
        value = None
        if name == "attr":
            value = self.value()
            self.expect(",")
        pairs = []
        while self.peek() != ")":
            k = self.key()
            self.expect("=")
            pairs.append((k, self.value()))
            self.ws()
            if self.peek() == ",":
                self.i += 1
        self.expect(")")
        return ("attr", value, pairs) if name == "attr" else ("dict", pairs)

    def err(self):
        self.expect("(")
        parts = [self.string()]
        self.expect(",")
        parts.append(self.string())
        self.expect(",")
        parts.append(self.string())
        index = None
        self.ws()
        if self.peek() == ",":
            self.i += 1
            if self.token("=") != "index":
                raise ValueError("bad err field")
            self.expect("=")
            index = int(self.token(")"))
        self.expect(")")
        return ("err", parts[0], parts[1], parts[2], index)

    def task(self):
        self.expect("(")
        target = int(self.token(","))
        self.expect(",")
        kind = int(self.token(","))
        self.expect(",")
        ident = int(self.token(","))
        self.expect(",")
        fields = []
        while self.peek() != ")":
            fields.append(self.value())
            self.ws()
            if self.peek() == ",":
                self.i += 1
        self.expect(")")
        return ("task", target, kind, ident, fields)


def parse(text):
    p = _Parser(text)
    v = p.value()
    p.ws()
    if p.i != len(p.s):
        raise ValueError("trailing notation at %d of %r" % (p.i, text))
    return v


# --- the encoder -------------------------------------------------------------

def _utf8(s):
    try:
        return s.encode("utf-8", "strict")
    except UnicodeEncodeError:
        raise Decline("a string is not valid UTF-8 (a lone surrogate)")


def _bare_str(s):
    b = _utf8(s)
    return struct.pack("<i", len(b)) + b


def encode(v, depth=0):
    tag = v[0]
    if tag == "nil":
        return b"\x00"
    if tag == "lgl":
        return b"\x01" + bytes([v[1]])
    if tag == "int":
        n = v[1]
        if n is None:
            n = INT64_MIN
        if not INT64_MIN <= n <= INT64_MAX:
            raise Decline("an int past int64")
        return b"\x02" + struct.pack("<q", n)
    if tag == "real":
        return b"\x03" + struct.pack("<Q", v[1])
    if tag == "cplx":
        return b"\x10" + struct.pack("<QQ", v[1], v[2])
    if tag == "str":
        if v[1] is None:
            return b"\x04" + struct.pack("<i", -1)
        b = _utf8(v[1])
        return b"\x04" + struct.pack("<i", len(b)) + b
    if tag == "bytes":
        return b"\x05" + struct.pack("<Q", len(v[1])) + v[1]
    if tag == "vec":
        name, elts = v[1], v[2]
        out = bytes([VEC_TAGS[name]]) + struct.pack("<Q", len(elts))
        if name == "lglv":
            return out + b"".join(struct.pack("<i", INT_MIN if e == 2 else e)
                                  for e in elts)
        if name == "intv":
            return out + b"".join(
                struct.pack("<i", INT_MIN if e is None else e) for e in elts)
        if name == "realv":
            return out + b"".join(struct.pack("<Q", e) for e in elts)
        if name == "cplxv":
            return out + b"".join(struct.pack("<QQ", *e) for e in elts)
        if name == "rawv":
            return out + bytes(elts)
        return out + b"".join(
            struct.pack("<q", INT64_MIN if e is None else e) for e in elts)
    if tag == "strv":
        elts = v[1]
        out = b"\x0b" + struct.pack("<Q", len(elts))
        for e in elts:
            out += struct.pack("<i", -1) if e is None else _bare_str(e)
        return out
    if tag in ("list", "tuple"):
        # a tuple writes the list tag: no reader materializes one
        return b"\x0c" + struct.pack("<Q", len(v[1])) + \
            b"".join(encode(e, depth + 1) for e in v[1])
    if tag == "dict":
        return _enc_dict(v[1], depth)
    if tag == "attr":
        return b"\x0f" + encode(v[1], depth + 1) + _enc_dict(v[2], depth + 1)
    if tag == "err":
        _, t, m, d, index = v
        out = b"\x11" + struct.pack("<H", 1 if index is not None else 0)
        if index is not None:
            out += struct.pack("<Q", index)
        return out + _bare_str(t) + _bare_str(m) + _bare_str(d)
    if tag == "task":
        _, target, kind, ident, fields = v
        if kind not in TASK_ARITY:
            raise Decline("unknown task kind")
        if len(fields) != TASK_ARITY[kind]:
            raise Decline("a task with the wrong field count")
        return b"\x12" + bytes([target, kind]) + b"\x00\x00" + \
            struct.pack("<Q", ident) + \
            b"".join(encode(f, depth + 1) for f in fields)
    raise Decline("unknown notation node %r" % (tag,))


def _enc_dict(pairs, depth):
    keys = set()
    for k, _ in pairs:
        if not isinstance(k, str):
            raise Decline("a non-str dict key")
        if k in keys:
            raise Decline("duplicate dict key %r" % k)
        keys.add(k)
    out = b"\x0d" + struct.pack("<Q", len(pairs))
    for k, val in pairs:
        out += _bare_str(k) + encode(val, depth + 1)
    return out


def encode_stream(v):
    return b"\x49" + bytes([VERSION]) + encode(v)


# --- the reader (the scan, then the structural builder) -----------------------

class _Reader:
    """The cursor walk (scan) and, on build, the structural home. The
    scan enforces the byte grammar: bounds, the depth cap, UTF-8, the
    attr positions, the top-level-only err, the reserved flag words, and
    the task arity. The builder adds only the duplicate-key rejection;
    every shape check stays with a binding's builder."""

    def __init__(self, data, build=True):
        self.d = data
        self.i = 0
        self.build = build

    def take(self, n, what="value"):
        if self.i + n > len(self.d):
            raise IxError("truncated interop stream")
        b = self.d[self.i:self.i + n]
        self.i += n
        return b

    def u64(self):
        return struct.unpack("<Q", self.take(8))[0]

    def bare_str(self, na_ok):
        (n,) = struct.unpack("<i", self.take(4))
        if n < -1:
            raise IxError("a string length below -1")
        if n == -1:
            if not na_ok:
                raise IxError("a dict key has length -1")
            return None
        b = self.take(n)
        try:
            return b.decode("utf-8", "strict")
        except UnicodeDecodeError:
            raise IxError("invalid UTF-8")

    def value(self, depth):
        if depth > DEPTH_MAX:
            raise IxError("past the depth cap")
        tag = self.take(1)[0]
        if tag == 0x00:
            return ("nil",)
        if tag == 0x01:
            v = self.take(1)[0]
            if v > 2:
                raise IxError("an lgl1 value past 2")
            return ("lgl", v)
        if tag == 0x02:
            (v,) = struct.unpack("<q", self.take(8))
            return ("int", None if v == INT64_MIN else v)
        if tag == 0x03:
            return ("real", self.u64())
        if tag == 0x10:
            return ("cplx", self.u64(), self.u64())
        if tag == 0x04:
            return ("str", self.bare_str(True))
        if tag == 0x05:
            return ("bytes", self.take(self.u64()))
        if tag in (0x06, 0x07, 0x08, 0x09, 0x0a, 0x0e):
            return self.vec(tag)
        if tag == 0x0b:
            n = self.u64()
            if n > len(self.d) - self.i:
                raise IxError("truncated interop stream")
            return ("strv", [self.bare_str(True) for _ in range(n)])
        if tag in (0x0c, 0x0d):
            n = self.u64()
            if n > len(self.d) - self.i:
                raise IxError("truncated interop stream")
            if tag == 0x0c:
                return ("list", [self.value(depth + 1) for _ in range(n)])
            keys = set()
            pairs = []
            for _ in range(n):
                k = self.bare_str(False)
                if self.build:
                    if k in keys:
                        raise IxError("duplicate dict key")
                    keys.add(k)
                pairs.append((k, self.value(depth + 1)))
            return ("dict", pairs)
        if tag == 0x0f:
            v = self.value(depth + 1)
            if v[0] == "attr":
                raise IxError("an attr wraps an attr")
            if self.take(1)[0] != 0x0d:
                raise IxError("the attr attributes are not a dict")
            self.i -= 1
            d = self.value(depth + 1)
            return ("attr", v, d[1])
        if tag == 0x11:
            if depth != 0:
                raise IxError("a nested err tag")
            (flags,) = struct.unpack("<H", self.take(2))
            if flags & ~1:
                raise IxError("unknown err flag bits set")
            index = self.u64() if flags & 1 else None
            return ("err", self.bare_str(False), self.bare_str(False),
                    self.bare_str(False), index)
        if tag == 0x12:
            hdr = self.take(12)
            target, kind = hdr[0], hdr[1]
            (flags,) = struct.unpack("<H", hdr[2:4])
            if flags != 0:
                raise IxError("unknown task flag bits set")
            if kind not in TASK_ARITY:
                raise IxError("unknown task kind 0x%02X" % kind)
            (ident,) = struct.unpack("<Q", hdr[4:12])
            return ("task", target, kind, ident,
                    [self.value(depth + 1)
                     for _ in range(TASK_ARITY[kind])])
        raise IxError("unsupported interop tag 0x%02X" % tag)

    def vec(self, tag):
        n = self.u64()
        width = {0x06: 4, 0x07: 4, 0x08: 8, 0x09: 16, 0x0a: 1, 0x0e: 8}[tag]
        if n > (len(self.d) - self.i) // width:
            raise IxError("truncated interop stream")
        name = {v: k for k, v in VEC_TAGS.items()}[tag]
        if tag == 0x0a:
            return ("vec", name, list(self.take(n)))
        elts = []
        for _ in range(n):
            if tag == 0x06:
                (v,) = struct.unpack("<i", self.take(4))
                elts.append(2 if v == INT_MIN else v)
            elif tag == 0x07:
                (v,) = struct.unpack("<i", self.take(4))
                elts.append(None if v == INT_MIN else v)
            elif tag == 0x08:
                elts.append(self.u64())
            elif tag == 0x09:
                elts.append((self.u64(), self.u64()))
            else:
                (v,) = struct.unpack("<q", self.take(8))
                elts.append(None if v == INT64_MIN else v)
        return ("vec", name, elts)


def scan(stream):
    r = _Reader(stream, build=False)
    r.take(2, "header")
    if stream[0] != 0x49:
        raise IxError("bad magic")
    if stream[1] != VERSION:
        raise IxError("interop format version 0x%02X" % stream[1])
    r.value(0)
    if r.i != len(stream):
        raise IxError("bytes past the one value")


def decode(stream):
    scan(stream)
    r = _Reader(stream)
    r.take(2, "header")
    return r.value(0)


def norm(v):
    """The value a decode returns: a tuple comes back as a list, and a
    literal -2^63 int comes back as NA (the 0x02 sentinel collision)."""
    tag = v[0]
    if tag == "int":
        return ("int", None) if v[1] == INT64_MIN else v
    if tag == "tuple":
        return ("list", [norm(e) for e in v[1]])
    if tag == "list":
        return ("list", [norm(e) for e in v[1]])
    if tag == "dict":
        return ("dict", [(k, norm(x)) for k, x in v[1]])
    if tag == "attr":
        return ("attr", norm(v[1]), [(k, norm(x)) for k, x in v[2]])
    if tag == "task":
        return ("task", v[1], v[2], v[3], [norm(e) for e in v[4]])
    return v


def has_tuple(v):
    if v[0] == "tuple":
        return True
    if v[0] in ("list",):
        return any(has_tuple(e) for e in v[1])
    if v[0] in ("dict",):
        return any(has_tuple(x) for _, x in v[1])
    if v[0] == "attr":
        return has_tuple(v[1]) or any(has_tuple(x) for _, x in v[2])
    return False


# --- the corpus ---------------------------------------------------------------

def run(cases_path, corpus_path, check=False):
    out = ["# Generated by tools/interop_corpus.py from tests/interop/cases.txt — do not edit.",
           "# id | hex (the whole stream, header included)"]
    n = 0
    for lineno, raw in enumerate(open(cases_path, encoding="utf-8"), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split("|")]
        if len(parts) not in (4, 5):
            raise ValueError("line %d: want 4-5 fields" % lineno)
        cid, kind, langs, value = parts[:4]
        note = parts[4] if len(parts) == 5 else ""
        if kind not in ("rt", "dec", "enc", "task", "read-err",
                        "write-decline"):
            raise ValueError("line %d: bad kind %r" % (lineno, kind))
        if langs not in ("all", "R", "PY"):
            raise ValueError("line %d: bad langs %r" % (lineno, langs))
        if kind == "read-err" and not note.startswith(("cursor:", "builder:")):
            raise ValueError("line %d: read-err notes start cursor:/builder:"
                             % lineno)

        hex_line = None
        if kind == "rt":
            v = parse(value)
            if has_tuple(v):
                raise ValueError("line %d: a tuple is never rt" % lineno)
            stream = encode_stream(v)
            if decode(stream) != v:
                raise ValueError("line %d: rt self-roundtrip failed" % lineno)
            hex_line = stream.hex()
        elif kind == "dec":
            # wire => home; without " => " the home is the wire value
            # itself (the usual case: the read shifts nothing expressible)
            wire, _, home = value.partition(" => ")
            w = parse(wire)
            parse(home if home else wire)    # syntax only; the binding asserts
            stream = encode_stream(w)
            if decode(stream) != norm(w):
                raise ValueError("line %d: dec wire side fails" % lineno)
            hex_line = stream.hex()
        elif kind == "enc":
            v = parse(value)
            stream = encode_stream(v)
            if decode(stream) != norm(v):
                raise ValueError("line %d: enc decode mismatch" % lineno)
            hex_line = stream.hex()
        elif kind == "task":
            v = parse(value)
            if v[0] != "task":
                raise ValueError("line %d: a task row's value is a task(...)"
                                 % lineno)
            stream = encode_stream(v)
            if decode(stream) != norm(v):
                raise ValueError("line %d: task self-roundtrip failed"
                                 % lineno)
            hex_line = stream.hex()
        elif kind == "read-err":
            if value.startswith("hex "):
                stream = bytes.fromhex(value[4:])
                if note.startswith("cursor:"):
                    try:
                        scan(stream)
                        raise ValueError("line %d: cursor row scans clean"
                                         % lineno)
                    except IxError:
                        pass
                else:
                    scan(stream)             # builder rows parse clean here
            else:
                v = parse(value)
                stream = encode_stream(v)
                scan(stream)
            hex_line = stream.hex()
        else:  # write-decline: nothing is written, so no corpus entry
            v = parse(value)
            try:
                encode(v)
                judged = False               # format-valid: binding policy
            except Decline:
                judged = True
            if not judged and not note.endswith("(binding)"):
                pass                         # a format-valid decline
        if hex_line is not None:
            out.append("%s | %s" % (cid, hex_line))
            n += 1

    text = "\n".join(out) + "\n"
    if check:
        import io
        with io.open(corpus_path, encoding="utf-8") as f:
            current = f.read()
        if current != text:
            print("interop corpus: %s is stale — regenerate with "
                  "tools/interop_corpus.py" % corpus_path, file=sys.stderr)
            return 1
        print("interop corpus: up to date (%d rows)" % n)
        return 0
    with open(corpus_path, "w", encoding="utf-8") as f:
        f.write(text)
    print("interop corpus: wrote %d rows to %s" % (n, corpus_path))
    return 0


if __name__ == "__main__":
    import os
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir)
    cases = os.path.join(root, "tests", "interop", "cases.txt")
    corpus = os.path.join(root, "tests", "interop", "corpus.txt")
    sys.exit(run(cases, corpus, check="--check" in sys.argv))
