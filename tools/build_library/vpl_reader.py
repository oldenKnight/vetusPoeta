"""Independent .vpl decoder (DESIGN.md section 5) for tests and vpl_inspect.py. It does not import pack.py: every
offset is computed here from the spec, so a packer bug cannot hide behind a shared helper. Read-only; uses mmap
for large files. Raises ValueError on any inconsistency (bad magic, sizes, offsets out of range, unsorted keys)."""
import bisect
import hashlib
import mmap
import struct

TAGS_ORDER = ("NOTE", "STRS", "KEYS", "ANAL", "LEMM", "SENS", "FEAT", "GENX", "REVX")
REQUIRED = ("NOTE", "STRS", "KEYS", "ANAL", "LEMM", "FEAT")
LEMM_FIELDS = ("head_off", "key_off", "pos", "cls", "gender", "tier", "freq_rank", "whit_freq", "tier_source",
               "emoji_off", "gloss_en_off", "gloss_es_off", "senses_start", "senses_count", "flags", "gen_start",
               "gen_count", "principal_off_count", "principal_off")


class _Keys(object):
    """Sequence view of the sorted key strings (as bytes) for bisect."""

    def __init__(self, r, base, n):
        self.r, self.base, self.n = r, base, n

    def __len__(self):
        return self.n

    def __getitem__(self, i):
        return self.r.str_bytes(self.r.u32(self.base + 4 * i))


class VplReader(object):
    def __init__(self, path=None, data=None, check_sha=False):
        self._f = None
        if data is None:
            self._f = open(path, "rb")
            self.buf = mmap.mmap(self._f.fileno(), 0, access=mmap.ACCESS_READ)
        else:
            self.buf = data
        b = self.buf
        if len(b) < 256 or b[0:4] != b"VPLX":
            raise ValueError("bad magic")
        self.major, self.minor = struct.unpack_from("<HH", b, 4)
        self.lang = bytes(b[8:16]).rstrip(b"\0").decode("ascii")
        n_sec = struct.unpack_from("<I", b, 16)[0]
        self.file_size = struct.unpack_from("<Q", b, 20)[0]
        self.sha256 = bytes(b[28:60])
        if self.file_size != len(b):
            raise ValueError("file_size %d != actual %d" % (self.file_size, len(b)))
        if any(b[60:256]):
            raise ValueError("header padding is not zero")
        if check_sha and hashlib.sha256(b[256:]).digest() != self.sha256:
            raise ValueError("sha256 mismatch")
        self.sections = []
        self.sec = {}
        end = 256 + 24 * n_sec
        for k in range(n_sec):
            tag, res, off, ln = struct.unpack_from("<4sIQQ", b, 256 + 24 * k)
            tag = tag.decode("ascii")
            if res != 0 or off % 8 or off < end or off + ln > len(b):
                raise ValueError("bad section %s" % tag)
            self.sections.append((tag, off, ln))
            self.sec[tag] = (off, ln)
        for t in REQUIRED:
            if t not in self.sec:
                raise ValueError("missing section %s" % t)
        so, sl = self.sec["STRS"]
        self.strs_off, self.strs_len = so, sl
        if sl < 1 or b[so] != 0 or b[so + sl - 1] != 0:
            raise ValueError("STRS must start and end with NUL")
        ko, _ = self.sec["KEYS"]
        self.n_keys = self.u32(ko)
        self.key_offs = ko + 4
        self.anal_start = ko + 4 + 4 * self.n_keys
        ao, _ = self.sec["ANAL"]
        self.n_anal = self.u32(ao)
        self.anal_base = ao + 4
        lo, _ = self.sec["LEMM"]
        self.n_lemmas = self.u32(lo)
        self.lemm_base = lo + 4
        fo, _ = self.sec["FEAT"]
        self.n_feat = self.u32(fo)
        self.feat_base = fo + 4
        self.n_senses = self.u32(self.sec["SENS"][0]) if "SENS" in self.sec else 0
        self.n_cells = self.u32(self.sec["GENX"][0]) if "GENX" in self.sec else 0
        if "REVX" in self.sec:
            ro = self.sec["REVX"][0]
            self.n_kw = self.u32(ro)
            self.kw_offs = ro + 4
            self.cand_start = ro + 4 + 4 * self.n_kw
            self.cand_base = self.cand_start + 4 * (self.n_kw + 1)
            self.n_cand = self.u32(self.cand_start + 4 * self.n_kw)
        else:
            self.n_kw = self.n_cand = 0
        self._check_sizes()

    def _check_sizes(self):
        def need(tag, size):
            if self.sec[tag][1] != size:
                raise ValueError("%s length %d, expected %d" % (tag, self.sec[tag][1], size))
        need("KEYS", 4 + 4 * self.n_keys + 4 * (self.n_keys + 1))
        need("ANAL", 4 + 12 * self.n_anal)
        need("LEMM", 4 + 48 * self.n_lemmas)
        need("FEAT", 4 + 4 * self.n_feat)
        if "SENS" in self.sec:
            need("SENS", 4 + 16 * self.n_senses)
        if "GENX" in self.sec:
            need("GENX", 4 + 8 * self.n_cells)
        if "REVX" in self.sec:
            need("REVX", 4 + 4 * self.n_kw + 4 * (self.n_kw + 1) + 8 * self.n_cand)
        if self.u32(self.anal_start + 4 * self.n_keys) != self.n_anal:
            raise ValueError("anal_start[n_keys] != n_anal")

    def close(self):
        if self._f is not None:
            self.buf.close()
            self._f.close()
            self._f = None

    # -- primitives -------------------------------------------------------------------------------------------
    def u32(self, at):
        return struct.unpack_from("<I", self.buf, at)[0]

    def str_bytes(self, off):
        if off >= self.strs_len:
            raise ValueError("string offset %d out of STRS" % off)
        a = self.strs_off + off
        e = self.buf.find(b"\0", a, self.strs_off + self.strs_len)
        return bytes(self.buf[a:e])

    def s(self, off):
        return self.str_bytes(off).decode("utf-8")

    def notice(self):
        o, n = self.sec["NOTE"]
        return bytes(self.buf[o:o + n]).decode("utf-8")

    # -- tables -----------------------------------------------------------------------------------------------
    def key(self, i):
        return self.s(self.u32(self.key_offs + 4 * i))

    def feature(self, fid):
        return self.u32(self.feat_base + 4 * fid)

    def feat_id(self, packed):
        lo, hi = 0, self.n_feat
        while lo < hi:
            mid = (lo + hi) // 2
            if self.feature(mid) < packed:
                lo = mid + 1
            else:
                hi = mid
        return lo if lo < self.n_feat and self.feature(lo) == packed else None

    def analysis(self, i):
        lemma, fid, flags, d = struct.unpack_from("<IHHI", self.buf, self.anal_base + 12 * i)
        return {"lemma": lemma, "feat_id": fid, "feat": self.feature(fid), "flags": flags, "display": self.s(d)}

    def lookup(self, key):
        kb = key.encode("utf-8")
        keys = _Keys(self, self.key_offs, self.n_keys)
        i = bisect.bisect_left(keys, kb)
        if i >= self.n_keys or keys[i] != kb:
            return []
        a, e = self.u32(self.anal_start + 4 * i), self.u32(self.anal_start + 4 * (i + 1))
        return [self.analysis(j) for j in range(a, e)]

    def lemma(self, i):
        if i >= self.n_lemmas:
            raise ValueError("lemma id %d out of range" % i)
        v = struct.unpack_from("<IIBBBBHBBIIIIHHIHHI", self.buf, self.lemm_base + 48 * i)
        r = dict(zip(LEMM_FIELDS, v))
        r["id"] = i
        r["head"] = self.s(r["head_off"])
        r["key"] = self.s(r["key_off"])
        r["emoji"] = self.s(r["emoji_off"])
        r["gloss_en"] = self.s(r["gloss_en_off"])
        r["gloss_es"] = self.s(r["gloss_es_off"])
        r["principal"] = self.s(r["principal_off"])
        return r

    def senses(self, i):
        l = self.lemma(i)
        if "SENS" not in self.sec:
            return []
        base = self.sec["SENS"][0] + 4
        out = []
        for k in range(l["senses_start"], l["senses_start"] + l["senses_count"]):
            ge, gs, kw, tags, rank = struct.unpack_from("<IIIHH", self.buf, base + 16 * k)
            out.append({"gloss_en": self.s(ge), "gloss_es": self.s(gs), "keywords": self.s(kw), "tags": tags,
                        "rank": rank})
        return out

    def cells(self, i):
        l = self.lemma(i)
        if "GENX" not in self.sec:
            return []
        base = self.sec["GENX"][0] + 4
        out = []
        for k in range(l["gen_start"], l["gen_start"] + l["gen_count"]):
            fid, res, form = struct.unpack_from("<HHI", self.buf, base + 8 * k)
            out.append((self.feature(fid), self.s(form), fid, res))
        return out

    def generate(self, i, packed):
        for f, form, _, _ in self.cells(i):
            if f == packed:
                return form
        return None

    def keyword(self, i):
        return self.s(self.u32(self.kw_offs + 4 * i))

    def reverse(self, kw):
        if not self.n_kw:
            return []
        kb = kw.encode("utf-8")
        keys = _Keys(self, self.kw_offs, self.n_kw)
        i = bisect.bisect_left(keys, kb)
        if i >= self.n_kw or keys[i] != kb:
            return []
        a, e = self.u32(self.cand_start + 4 * i), self.u32(self.cand_start + 4 * (i + 1))
        out = []
        for j in range(a, e):
            lemma, sense, score, pos = struct.unpack_from("<IHBB", self.buf, self.cand_base + 8 * j)
            out.append({"lemma": lemma, "sense": sense, "score": score, "pos": pos})
        return out

    def validate(self):
        """Full structural walk (slow on big files): sorted keys, monotone starts, offsets in range."""
        prev = None
        for i in range(self.n_keys):
            k = self.str_bytes(self.u32(self.key_offs + 4 * i))
            if prev is not None and not prev < k:
                raise ValueError("KEYS not strictly sorted at %d" % i)
            prev = k
            if self.u32(self.anal_start + 4 * i) > self.u32(self.anal_start + 4 * (i + 1)):
                raise ValueError("anal_start not monotone at %d" % i)
        for i in range(self.n_anal):
            lemma, fid, _fl, d = struct.unpack_from("<IHHI", self.buf, self.anal_base + 12 * i)
            if lemma >= self.n_lemmas or fid >= self.n_feat or d >= self.strs_len:
                raise ValueError("ANAL record %d out of range" % i)
        feats = [self.feature(i) for i in range(self.n_feat)]
        if feats != sorted(set(feats)):
            raise ValueError("FEAT not strictly ascending")
        for i in range(self.n_lemmas):
            l = self.lemma(i)
            if l["senses_start"] + l["senses_count"] > self.n_senses or l["gen_start"] + l["gen_count"] > self.n_cells:
                raise ValueError("lemma %d ranges out of SENS/GENX" % i)
            fids = [c[2] for c in self.cells(i)]
            if fids != sorted(fids):
                raise ValueError("GENX of lemma %d not sorted by feat_id" % i)
        prev = None
        for i in range(self.n_kw):
            k = self.str_bytes(self.u32(self.kw_offs + 4 * i))
            if prev is not None and not prev < k:
                raise ValueError("REVX keywords not strictly sorted at %d" % i)
            prev = k
            cs = self.reverse(k.decode("utf-8"))
            order = [(-c["score"], c["lemma"], c["sense"]) for c in cs]
            if order != sorted(order):
                raise ValueError("candidates of %r not sorted" % k)
            for c in cs:
                if c["lemma"] >= self.n_lemmas:
                    raise ValueError("candidate lemma out of range")
        return True
