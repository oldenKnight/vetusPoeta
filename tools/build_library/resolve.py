"""Stage `resolve`: join table forms and form pages to lemma ids and write the analyses.

Inputs (from the kaikki stage) in <out>/<lang>/: lemmas.jsonl, table_forms.tsv, formpages.tsv.
Outputs in <out>/<lang>/:
  analyses.tsv     key, lemma_id, packed_features, display, flags   (sorted bytewise by key, lemma, features,
                   display; duplicates merged by OR-ing flags)
  lemma_index.tsv  lemma_id, key, head, pos, has_table, kind, ht
ANAL flags (DESIGN section 5): bit0 inflection table, bit1 form-of page, bit3 alternative spelling,
bit4 non-Attic / dialectal, bit5 Medieval/Late/New Latin, bit6 poetic/rare/archaic.
Greek tables are grouped by their table-tags marker row ("Attic declension-2", "Ionic contracted present"):
tables naming no dialect or naming Attic are kept as Attic (extra attic bit when named), others get bit4;
an uncontracted table that has a contracted twin (present / contracted present) also gets bit4, and the
contracted twin gets the extra contracted bit. Greek verb cells carry no tense: it comes from the marker.
"""
import collections
import heapq
import json
import os
import tempfile
import time

import features as F
import tagmap
import vptext
from common import TsvWriter, log, peak_rss_mb, read_tsv

FLAG_TABLE, FLAG_FORMPAGE, FLAG_WHITAKER, FLAG_ALT, FLAG_DIALECT, FLAG_LATE, FLAG_RARE, FLAG_ENCLITIC = (
    1, 2, 4, 8, 16, 32, 64, 128)
RARE_TAGS = frozenset(("poetic", "rare", "archaic", "obsolete", "dated", "uncommon"))
MAX_DEPTH = 3
CHUNK_ROWS = 1500000

TENSE_WORDS = ("pluperfect", "perfect", "aorist", "imperfect", "present", "future")


def parse_marker(marker):
    """Greek/Latin table marker -> (tense_hint or None, dialect tokens, contracted?, token frozenset)."""
    toks = marker.split()
    dialects = frozenset(t for t in toks if t[:1].isupper())
    tense = None
    ts = set(t for t in toks if t in TENSE_WORDS)
    if "future" in ts and "perfect" in ts and _adjacent(toks, "future", "perfect"):
        tense = "future-perfect"
    else:
        for w in TENSE_WORDS:  # priority order: pluperfect > perfect > aorist > imperfect > present > future
            if w in ts:
                tense = w
                break
    return tense, dialects, "contracted" in toks, frozenset(toks)


def _adjacent(toks, a, b):
    return any(toks[i] == a and toks[i + 1] == b for i in range(len(toks) - 1))


def dialect_flags(dialects):
    """(flags, extra) for a set of dialect tokens."""
    if not dialects:
        return 0, 0
    if "Attic" in dialects:
        return 0, F.EXTRA["attic"]
    if dialects & tagmap.GREEK_DIALECTS:
        return FLAG_DIALECT, 0
    return 0, 0


def tag_flags(tags, lang):
    tags = [tagmap.DIALECT_ALIASES.get(t, t) for t in tags]
    fl = 0
    if lang == "la" and any(t in tagmap.LATIN_ERAS for t in tags):
        fl |= FLAG_LATE
    if lang == "grc" and "Attic" not in tags and any(t in tagmap.GREEK_DIALECTS for t in tags):
        fl |= FLAG_DIALECT
    if any(t in RARE_TAGS for t in tags):
        fl |= FLAG_RARE
    if "alternative" in tags:
        fl |= FLAG_ALT
    return fl


def merge_lemma_flags(fl, lemma_fl):
    """A form page of a Late Latin or alternative-spelling lemma inherits those qualifiers."""
    return fl | lemma_fl


def compatible(fp_pos, lemma_pos):
    if fp_pos == lemma_pos:
        return True
    pair = {fp_pos, lemma_pos}
    return pair <= {"verb", "participle"} or pair <= {"adj", "participle"} or pair <= {"noun", "name"}


PROVENANCE_BITS = FLAG_TABLE | FLAG_FORMPAGE | FLAG_WHITAKER | FLAG_ENCLITIC
QUALIFIER_BITS = FLAG_ALT | FLAG_DIALECT | FLAG_LATE | FLAG_RARE


def merge_flags(a, b):
    """Provenance bits are OR-ed (any source counts); qualifier bits are AND-ed: a form found in an Attic table and
    in an Ionic table is not dialectal, a form that is also a plain spelling is not an alternative spelling."""
    return ((a | b) & PROVENANCE_BITS) | (a & b & QUALIFIER_BITS)


def fold_attic(group):
    """group: list of [packed, display, flags] for one (key, lemma). A form-page analysis that lacks only the extra
    attic bit of a table analysis is folded into that analysis (form pages do not name the dialect)."""
    attic = F.EXTRA["attic"] << 27
    by = {(g[0], g[1]): g for g in group}
    out = []
    for g in group:
        twin = by.get((g[0] | attic, g[1]))
        if not (g[0] & attic) and twin is not None and twin is not g and not (g[2] & FLAG_TABLE):
            twin[2] = merge_flags(twin[2], g[2])
            continue
        out.append(g)
    return out


class Sorter(object):
    """External sort of analysis rows with flag merging; memory is bounded by CHUNK_ROWS."""

    def __init__(self, tmpdir):
        self.tmpdir = tmpdir
        self.buf = {}
        self.chunks = []
        self.added = 0

    def add(self, key, lemma, packed, display, flags):
        k = (key, lemma, packed, display)
        old = self.buf.get(k)
        self.buf[k] = flags if old is None else merge_flags(old, flags)
        self.added += 1
        if len(self.buf) >= CHUNK_ROWS:
            self._spill()

    def _spill(self):
        fd, path = tempfile.mkstemp(prefix="anal-", suffix=".tsv", dir=self.tmpdir)
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            for k in sorted(self.buf):
                f.write("%s\t%d\t%d\t%s\t%d\n" % (k[0], k[1], k[2], k[3], self.buf[k]))
        self.chunks.append(path)
        self.buf = {}

    def _read(self, path):
        for parts in read_tsv(path):
            yield (parts[0], int(parts[1]), int(parts[2]), parts[3]), int(parts[4])

    def merged(self):
        mem = sorted(self.buf.items())
        self.buf = {}
        iters = [iter(mem)] + [self._read(p) for p in self.chunks]
        cur = None
        curf = 0
        for k, fl in heapq.merge(*iters, key=lambda kv: kv[0]):
            if k == cur:
                curf = merge_flags(curf, fl)
                continue
            if cur is not None:
                yield cur, curf
            cur, curf = k, fl
        if cur is not None:
            yield cur, curf
        for p in self.chunks:
            os.unlink(p)
        self.chunks = []


def run(lang, out_dir):
    t0 = time.time()
    d = os.path.join(out_dir, lang)
    key_fn = vptext.key_for(lang)
    stats = tagmap.TagStats()
    c = collections.Counter()

    # -- lemmas ---------------------------------------------------------------------------------
    lemma_by_key = {}
    lemma_pos = []
    lemma_word = []
    lemma_key = []
    lemma_flags = []
    lemma_head = []
    idx = TsvWriter(os.path.join(d, "lemma_index.tsv"))
    with open(os.path.join(d, "lemmas.jsonl"), "r", encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            i = r["id"]
            if i != len(lemma_pos):
                raise ValueError("lemma ids are not sequential at %d" % i)
            lemma_by_key.setdefault(r["key"], []).append(i)
            lemma_pos.append(r["fpos"])
            lemma_word.append(vptext.strip_length_marks(r["word"]))
            lemma_key.append(r["key"])
            lemma_head.append(r["head"])
            fl = 0
            if r.get("kind") == "alttable":
                fl |= FLAG_ALT
            if r.get("late"):
                fl |= FLAG_LATE
            lemma_flags.append(fl)
            idx.write(i, r["key"], r["head"], r["pos"], 1 if r.get("has_table") else 0, r.get("kind", ""),
                      r.get("ht", ""))
    idx.close()
    c["lemmas"] = len(lemma_pos)
    log("[%s resolve] %d lemmas loaded" % (lang, len(lemma_pos)))

    sorter = Sorter(d)
    self_covered = set()

    # -- table forms ----------------------------------------------------------------------------
    def flush_lemma(lid, rows):
        if lid is None:
            return
        parsed = {}
        contracted_twins = set()
        for row in rows:
            m = row[5] if len(row) > 5 else ""
            if m not in parsed:
                parsed[m] = parse_marker(m)
                if parsed[m][2]:
                    contracted_twins.add(parsed[m][3] - {"contracted"})
        pos = lemma_pos[lid]
        base = lemma_flags[lid]
        for row in rows:
            _, display, key, tags, source, marker = (row + [""] * 6)[:6]
            tl = tags.split() if tags else []
            tense, dialects, contracted, toks = parsed[marker]
            fl = base | tag_flags(tl, lang)
            fl |= FLAG_TABLE if source != "head" else 0
            extra = 0
            if lang == "grc":
                dfl, dex = dialect_flags(dialects)
                fl |= dfl
                extra |= dex
                if contracted:
                    extra |= F.EXTRA["contracted"]
                elif toks in contracted_twins and "Attic" not in dialects:
                    fl |= FLAG_DIALECT
            hint = tense if lang == "grc" else None
            for packed in tagmap.tags_to_feature_list(tl, pos, tense_hint=hint, extra=extra, stats=stats):
                sorter.add(key, lid, packed, display, fl)
                c["table_analyses_raw"] += 1
            if key == lemma_key[lid]:
                self_covered.add(lid)

    cur, rows = None, []
    for row in read_tsv(os.path.join(d, "table_forms.tsv")):
        c["table_rows"] += 1
        lid = int(row[0])
        if lid != cur:
            flush_lemma(cur, rows)
            cur, rows = lid, []
        rows.append(row)
        if c["table_rows"] % 1000000 == 0:
            log("[%s resolve] %d table rows" % (lang, c["table_rows"]))
    flush_lemma(cur, rows)

    # -- lemma self analyses (lemmas whose own key is not among their table forms) --------------
    for lid in range(len(lemma_pos)):
        if lid not in self_covered:
            sorter.add(lemma_key[lid], lid, F.pack(pos=F.POS[lemma_pos[lid]]), lemma_head[lid], lemma_flags[lid])
            c["lemma_self_analyses"] += 1

    # -- form pages -----------------------------------------------------------------------------
    fp_path = os.path.join(d, "formpages.tsv")
    fp_targets = {}
    for row in read_tsv(fp_path):
        fp_targets.setdefault(row[1], set()).add(row[3])
    log("[%s resolve] %d form-page keys indexed" % (lang, len(fp_targets)))

    def candidates(tkey, depth, seen):
        ids = lemma_by_key.get(tkey)
        if ids:
            return ids, depth
        if depth >= MAX_DEPTH or tkey in seen:
            return [], depth
        seen.add(tkey)
        out = []
        best = depth
        for t2 in sorted(fp_targets.get(tkey, ())):
            got, dd = candidates(t2, depth + 1, seen)
            for g in got:
                if g not in out:
                    out.append(g)
                    best = max(best, dd)
        return out, best

    unresolved_sample = []
    unresolved_pages = set()
    depth_hist = collections.Counter()
    for row in read_tsv(fp_path):
        word, key, display, tkey, tags, fpos, kind, target = (row + [""] * 8)[:8]
        c["formpage_rows"] += 1
        ids, depth = candidates(tkey, 1, set())
        if not ids:
            c["unresolved_rows"] += 1
            unresolved_pages.add(key)
            if len(unresolved_sample) < 30:
                unresolved_sample.append("%s -> %s" % (word, target))
            continue
        depth_hist[depth] += 1
        comp = [i for i in ids if compatible(fpos, lemma_pos[i])]
        if comp:
            ids = comp
        tw = vptext.strip_length_marks(target)
        exact = [i for i in ids if lemma_word[i] == tw]
        if exact:
            ids = exact
        tl = tags.split() if tags else []
        fl = FLAG_FORMPAGE | tag_flags(tl, lang)
        if kind == "alt":
            fl |= FLAG_ALT
        packs = tagmap.tags_to_feature_list(tl, fpos, stats=stats)
        for i in ids:
            lp = lemma_pos[i]
            for packed in packs:
                sorter.add(key, i, tagmap.align_pos(packed, lp), display, merge_lemma_flags(fl, lemma_flags[i]))
                c["formpage_analyses_raw"] += 1
    c["unresolved_pages"] = len(unresolved_pages)
    del fp_targets

    # -- write analyses -------------------------------------------------------------------------
    hist = collections.Counter()
    out = TsvWriter(os.path.join(d, "analyses.tsv"))
    flag_counts = collections.Counter()
    state = {"key": None, "n": 0}

    def emit(key, lemma, group):
        if lang == "grc" and len(group) > 1:
            group = fold_attic(group)
        for packed, display, fl in group:
            out.write(key, lemma, packed, display, fl)
            for b in range(8):
                if fl & (1 << b):
                    flag_counts["bit%d" % b] += 1
            if key != state["key"]:
                if state["key"] is not None:
                    hist[state["n"]] += 1
                state["key"], state["n"] = key, 0
            state["n"] += 1

    gk, group = None, []
    for (key, lemma, packed, display), fl in sorter.merged():
        if (key, lemma) != gk:
            if gk is not None:
                emit(gk[0], gk[1], group)
            gk, group = (key, lemma), []
        group.append([packed, display, fl])
    if gk is not None:
        emit(gk[0], gk[1], group)
    if state["key"] is not None:
        hist[state["n"]] += 1
    out.close()
    c["analyses"] = out.rows
    c["distinct_keys"] = sum(hist.values())
    buckets = collections.OrderedDict()
    for lo, hi, name in ((1, 1, "1"), (2, 2, "2"), (3, 3, "3"), (4, 4, "4"), (5, 9, "5-9"), (10, 19, "10-19"),
                         (20, 49, "20-49"), (50, 10 ** 9, "50+")):
        buckets[name] = sum(v for k, v in hist.items() if lo <= k <= hi)
    res = {"counts": dict(c), "analyses_per_key": buckets, "flags": dict(sorted(flag_counts.items())),
           "chain_depth": {str(k): v for k, v in sorted(depth_hist.items())},
           "unresolved_sample": unresolved_sample, "tags": stats.as_dict(),
           "duration_s": round(time.time() - t0, 1), "peak_rss_mb": peak_rss_mb()}
    log("[%s resolve] done: %s" % (lang, " ".join("%s=%s" % kv for kv in sorted(c.items()))))
    return res
