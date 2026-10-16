"""Writer and reader of the .vpt model format (DESIGN.md section 17). Little-endian, sections 8-byte aligned.

Header (256 bytes):
  0  char[4] magic "VPTX"         4  u16 major = 1       6  u16 minor = 0
  8  char[8] lang (NUL padded)   16  char[8] kind ("tag" | "dep", NUL padded)
 24  u32 n_labels                28  u32 n_feats         32  u32 table_size (power of two)   36  u32 reserved = 0
 40  u64 file_size               48  u8[32] sha256 of bytes [256, file_size)                  80  zero padding
LABELS  at 256: n_labels NUL-terminated UTF-8 strings, zero padded to a multiple of 8
FEATS   table_size x u64 FNV-1a 64 feature hashes, 0 = empty slot; linear probing from hash & (table_size - 1)
WEIGHTS table_size x n_labels x int16 (row i belongs to slot i; empty slots are zero rows), zero padded to 8
NOTE    UTF-8 text up to file_size
"""
import hashlib
import os
import struct

from features import fnv1a64

MAGIC = b"VPTX"
MAJOR = 1
MINOR = 0
HEADER = 256


def _align8(n):
    return (n + 7) & ~7


def build_table(rows, table_size):
    """rows: dict hash -> list of ints. Returns the slot list (hash or 0) in deterministic order
    (insertion by ascending hash)."""
    assert table_size & (table_size - 1) == 0
    assert len(rows) < table_size
    slots = [0] * table_size
    mask = table_size - 1
    for h in sorted(rows):
        i = h & mask
        while slots[i] != 0:
            i = (i + 1) & mask
        slots[i] = h
    return slots


def write(path, lang, kind, labels, feat_rows, table_size, note):
    """feat_rows: dict feature string -> list of n_labels ints in int16 range. Returns (size, sha256 hex)."""
    n_labels = len(labels)
    rows = {}
    for f, r in feat_rows.items():
        h = fnv1a64(f)
        if h in rows:
            raise ValueError("FNV-1a 64 collision on %r" % f)
        assert len(r) == n_labels
        rows[h] = r
    slots = build_table(rows, table_size)
    lab = b"".join(l.encode("utf-8") + b"\0" for l in labels)
    lab += b"\0" * (_align8(len(lab)) - len(lab))
    feats = struct.pack("<%dQ" % table_size, *slots)
    zero = [0] * n_labels
    flat = []
    for h in slots:
        flat.extend(rows[h] if h else zero)
    weights = struct.pack("<%dh" % len(flat), *flat)
    weights += b"\0" * (_align8(len(weights)) - len(weights))
    body = lab + feats + weights + note.encode("utf-8")
    size = HEADER + len(body)
    sha = hashlib.sha256(body).digest()
    hdr = struct.pack("<4sHH8s8sIIIIQ32s", MAGIC, MAJOR, MINOR, lang.encode()[:8], kind.encode()[:8],
                      n_labels, len(rows), table_size, 0, size, sha)
    hdr += b"\0" * (HEADER - len(hdr))
    tmp = path + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(hdr)
        fh.write(body)
    os.replace(tmp, path)
    return size, hashlib.sha256(hdr + body).hexdigest()


class Model:
    """A loaded .vpt: labels, rows (hash -> tuple of ints), header fields, note. Validates like the C++ reader."""

    def __init__(self, path):
        with open(path, "rb") as fh:
            data = fh.read()
        if len(data) < HEADER:
            raise ValueError("truncated header")
        (magic, major, minor, lang, kind, n_labels, n_feats, table_size, _res, size,
         sha) = struct.unpack_from("<4sHH8s8sIIIIQ32s", data, 0)
        if magic != MAGIC or major != MAJOR:
            raise ValueError("bad magic or version")
        if size != len(data):
            raise ValueError("size mismatch")
        if hashlib.sha256(data[HEADER:]).digest() != sha:
            raise ValueError("sha256 mismatch")
        self.lang = lang.rstrip(b"\0").decode()
        self.kind = kind.rstrip(b"\0").decode()
        self.minor = minor
        pos = HEADER
        labels = []
        for _ in range(n_labels):
            e = data.index(b"\0", pos)
            labels.append(data[pos:e].decode("utf-8"))
            pos = e + 1
        pos = _align8(pos)
        self.labels = labels
        slots = struct.unpack_from("<%dQ" % table_size, data, pos)
        pos += 8 * table_size
        w = struct.unpack_from("<%dh" % (table_size * n_labels), data, pos)
        pos = _align8(pos + 2 * table_size * n_labels)
        self.note = data[pos:].decode("utf-8")
        self.rows = {}
        for i, h in enumerate(slots):
            if h:
                self.rows[h] = w[i * n_labels:(i + 1) * n_labels]
        assert len(self.rows) == n_feats
        self.table_size = table_size
        self.n = n_labels
        self._cache = {}

    def scores(self, feats):
        s = [0] * self.n
        rows = self.rows
        cache = self._cache
        for f in feats:
            h = cache.get(f)
            if h is None:
                h = fnv1a64(f)
                if len(cache) < 2000000:
                    cache[f] = h
            r = rows.get(h)
            if r is not None:
                s = [a + b for a, b in zip(s, r)]
        return s
