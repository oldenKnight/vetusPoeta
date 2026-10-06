// orbergise() and cues() (vp/orberg.h): the rewrite of a cue sentence by sentence, the original-language path, the
// alignment that turns two texts into "was -> now" changes, the meaning check, the checks (A1-A4 from the Latin
// checker, A6 with participles counted as their verbs, A7 = meaning) and the confidence.
#include <algorithm>
#include <cmath>
#include <exception>

#include "internal.h"
#include "vp/check.h"
#include "vp/cue.h"
#include "vp/morph.h"
#include "vp/realise_la.h"
#include "vp/subs.h"
#include "vp/text.h"

namespace vp::orberg {

using namespace vp::feat;
using detail::kNone;

std::string reasonData(const Change& c) {
  return "{\"was\":\"" + detail::jsonEscape(c.from) + "\",\"now\":\"" + detail::jsonEscape(c.to) + "\",\"why\":\"" +
         detail::jsonEscape(c.why.empty() ? c.reason : c.why) + "\"}";
}

namespace {

struct TextOut {
  std::string text;
  std::vector<Change> changes;                         // tokenIndex = byte offset of the first new word (-1 none)
  std::vector<rules::Reason> notes;
  std::vector<std::string> flags;
  std::vector<std::pair<uint32_t, std::string>> content;
  std::vector<std::pair<uint32_t, uint32_t>> mapped;
  std::vector<uint32_t> inputLemmas;
  bool changed = false, macrons = false;
};

void addFlag(std::vector<std::string>& f, const std::string& x) {
  if (std::find(f.begin(), f.end(), x) == f.end()) f.push_back(x);
}

void rewriteText(const std::string& latin, const OrbergOptions& o, const EngineContext& ctx, Resources::Impl& R,
                 bool vocabOnly, TextOut& out) {
  const auto ranges = la2x::splitSentences(latin);
  size_t prev = 0;
  for (const auto& r : ranges) {
    if (r.first > prev) out.text += latin.substr(prev, r.first - prev);
    detail::SentenceIn in;
    in.text = latin.substr(r.first, r.second - r.first);
    in.opt = &o;
    in.ctx = &ctx;
    in.simplifyOnly = vocabOnly;
    detail::SentenceOut so;
    detail::rewriteSentence(in, R, so);
    const int base = (int)out.text.size();
    for (Change c : so.changes) {
      if (c.tokenIndex >= 0) c.tokenIndex += base;
      out.changes.push_back(std::move(c));
    }
    out.text += so.text;
    for (rules::Reason& n : so.notes) out.notes.push_back(std::move(n));
    for (const std::string& f : so.flags) addFlag(out.flags, f);
    out.content.insert(out.content.end(), so.content.begin(), so.content.end());
    out.mapped.insert(out.mapped.end(), so.mapped.begin(), so.mapped.end());
    out.inputLemmas.insert(out.inputLemmas.end(), so.inputLemmas.begin(), so.inputLemmas.end());
    out.changed = out.changed || so.changed;
    out.macrons = out.macrons || so.macrons;
    prev = r.second;
  }
  if (prev < latin.size()) out.text += latin.substr(prev);
}

// The output read back by the analyser: tokens with absolute offsets, every reading (meaning, A6, alignment).
struct Read {
  std::vector<la2x::Sentence> sents;
  std::vector<size_t> offsets;
};
void readBack(const std::string& t, const EngineContext& ctx, Read& out) {
  for (const auto& r : la2x::splitSentences(t)) {
    out.sents.emplace_back();
    ctx.la2x->analyser().analyse(std::string_view(t).substr(r.first, r.second - r.first), out.sents.back(), ctx.glossary);
    out.offsets.push_back(r.first);
  }
}

bool isName(const la2x::Token& t, const lex::Lexicon& la) {
  for (const la2x::Reading& r : t.readings)
    if (r.name || (r.lemma != kNone && (la.lemma(r.lemma).flags & lex::ProperName))) return true;
  return t.nameGuess;
}

// Word-level key for the alignment: the content lemma (participles -> verb), a name key, or the written key.
uint32_t alignKey(const la2x::Token& t, Resources::Impl& R) {
  if (t.readings.empty()) return detail::nameId(text::latin_key(t.text));
  const la2x::Reading& r = t.readings[0];
  if (r.name || r.lemma == kNone) return detail::nameId(text::latin_key(t.text));
  if (R.isParticipleLemma(r.lemma)) {
    const uint32_t v = R.verbOf(r.lemma);
    if (v != kNone) return v;
  }
  return r.lemma;
}

struct AWord { std::string text; uint32_t key = 0; int start = 0; uint8_t pos = 0; };
std::vector<AWord> alignWords(const Read& rd, Resources::Impl& R) {
  std::vector<AWord> w;
  for (size_t s = 0; s < rd.sents.size(); ++s)
    for (const la2x::Token& t : rd.sents[s].tokens) {
      if (t.kind != la2x::TokKind::Word) continue;
      AWord a;
      a.text = t.text;
      a.key = alignKey(t, R);
      a.start = t.start + (int)rd.offsets[s];
      a.pos = t.readings.empty() || t.readings[0].lemma == kNone ? 0 : R.la.lemma(t.readings[0].lemma).pos;
      w.push_back(a);
    }
  return w;
}

// "was -> now" from two texts: longest common subsequence on the word keys; each gap is one change (vocabulary for a
// one-word swap of the same part of speech, else structure), a kept lemma with another form is a structure change,
// a lemma that leaves one gap and enters another is an order change.
std::vector<Change> alignChanges(const std::vector<AWord>& a, const std::vector<AWord>& b) {
  const size_t n = a.size(), m = b.size();
  std::vector<std::vector<uint16_t>> L(n + 1, std::vector<uint16_t>(m + 1, 0));
  for (size_t i = n; i-- > 0;)
    for (size_t j = m; j-- > 0;)
      L[i][j] = a[i].key == b[j].key ? (uint16_t)(L[i + 1][j + 1] + 1) : std::max(L[i + 1][j], L[i][j + 1]);
  std::vector<Change> out;
  size_t i = 0, j = 0;
  auto flush = [&](size_t i0, size_t i1, size_t j0, size_t j1) {
    if (i0 == i1 && j0 == j1) return;
    Change c;
    for (size_t k = i0; k < i1; ++k) c.from += (c.from.empty() ? "" : " ") + a[k].text;
    for (size_t k = j0; k < j1; ++k) c.to += (c.to.empty() ? "" : " ") + b[k].text;
    c.reason = (i1 - i0 == 1 && j1 - j0 == 1 && a[i0].pos == b[j0].pos) ? "vocabulary" : "structure";
    c.rule = "original";
    c.why = c.reason == "vocabulary" ? "word from the original's translation" : "structure from the original's translation";
    c.tokenIndex = j1 > j0 ? b[j0].start : -1;
    out.push_back(c);
  };
  size_t gi = 0, gj = 0;
  while (i < n && j < m) {
    if (a[i].key == b[j].key) {
      flush(gi, i, gj, j);
      if (text::latin_key(a[i].text) != text::latin_key(b[j].text)) {
        Change c;
        c.from = a[i].text;
        c.to = b[j].text;
        c.reason = "structure";
        c.rule = "original";
        c.why = "same word, another form";
        c.tokenIndex = b[j].start;
        out.push_back(c);
      }
      ++i;
      ++j;
      gi = i;
      gj = j;
    } else if (L[i + 1][j] >= L[i][j + 1]) {
      ++i;
    } else {
      ++j;
    }
  }
  flush(gi, n, gj, m);
  // order: a word deleted in one change and inserted in another
  for (size_t x = 0; x < out.size(); ++x)
    for (size_t y = 0; y < out.size(); ++y) {
      if (x == y || out[x].from.empty() || out[y].to.empty()) continue;
      if (out[x].from == out[y].to) { out[x].reason = "order"; out[y].reason = "order"; out[y].why = "word order"; }
    }
  return out;
}

double meaningOf(const std::vector<std::pair<uint32_t, std::string>>& content0,
                 const std::vector<std::pair<uint32_t, uint32_t>>& mapped, const detail::LemmaSet& outSet,
                 std::vector<std::string>& missing) {
  std::vector<std::pair<uint32_t, std::string>> content = content0;
  std::sort(content.begin(), content.end());
  content.erase(std::unique(content.begin(), content.end(),
                            [](const std::pair<uint32_t, std::string>& x, const std::pair<uint32_t, std::string>& y) {
                              return x.first == y.first;
                            }),
                content.end());
  if (content.empty()) return 1.0;
  size_t ok = 0;
  for (const auto& c : content) {
    bool found = false;
    if (c.first & 0x80000000u) found = outSet.hasName(text::latin_key(c.second));
    else found = outSet.has(c.first);
    // substitutions (swaps, pairs, readings a rule chose), followed up to three steps
    std::vector<uint32_t> frontier{c.first};
    for (int step = 0; step < 3 && !found && !frontier.empty(); ++step) {
      std::vector<uint32_t> next;
      for (uint32_t f : frontier)
        for (const auto& m : mapped)
          if (m.first == f && m.second != f) {
            if (outSet.has(m.second)) found = true;
            next.push_back(m.second);
          }
      frontier.swap(next);
    }
    if (found) ++ok;
    else missing.push_back(c.second);
  }
  return (double)ok / (double)content.size();
}

std::string pct(double v) { return std::to_string((int)std::lround(v * 100.0)) + " %"; }

struct Verdict { std::vector<rules::Check> checks; bool fix = false, a6 = true; std::vector<rules::TokenView> tokens; };

// Checks on a candidate output: A1-A4 by the Latin checker, A6 here (participles with their verb's tier, names and
// numbers exempt), tokens for the UI.
Verdict verdict(const std::string& t, const Read& rd, const OrbergOptions& o, const EngineContext& ctx,
                Resources::Impl& R) {
  Verdict v;
  std::vector<check::TokenHint> hints;
  std::vector<std::string> over;
  bool unknown = false;
  for (size_t s = 0; s < rd.sents.size(); ++s)
    for (const la2x::Token& tk : rd.sents[s].tokens) {
      if (tk.kind != la2x::TokKind::Word) continue;
      const int st = tk.start + (int)rd.offsets[s], en = tk.end + (int)rd.offsets[s];
      rules::TokenView tv;
      tv.text = tv.display = tk.text;
      tv.start = st;
      tv.end = en;
      const bool name = isName(tk, R.la);
      const la2x::Reading* b = tk.best();
      if (b && b->lemma != kNone) {
        tv.lemmaId = b->lemma;
        tv.hasLemma = true;
        tv.features = realise::featureView(b->packed);
        tv.tier = R.wordTier(b->lemma);
      }
      tv.unknown = tk.unknown && !name;
      tv.fromRule = tk.fromRule;
      unknown = unknown || tv.unknown;
      v.tokens.push_back(tv);
      if (name) {
        check::TokenHint h;
        h.start = st;
        h.end = en;
        h.name = true;
        hints.push_back(h);
        continue;
      }
      uint8_t best = 9;
      for (const la2x::Reading& r : tk.readings)
        if (r.lemma != kNone) best = std::min(best, R.wordTier(r.lemma));
      if (best != 9 && best > o.tierCeiling) over.push_back(tk.text + " (tier " + std::to_string(best) + ")");
    }
  check::Options co;
  co.tierCeiling = (uint8_t)o.tierCeiling;
  co.glossary = ctx.glossary;
  co.hints = &hints;
  check::Report rep;
  ctx.checker->check(t, co, rep);
  for (const rules::Check& c : rep.checks) {
    if (c.id == "A6") continue;
    v.checks.push_back(c);
    if ((c.id == "A1" || c.id == "A3" || c.id == "A4") && !c.ok) v.fix = true;
  }
  rules::Check a6{"A6", over.empty(), ""};
  if (!over.empty()) {
    a6.detail = "above tier " + std::to_string(o.tierCeiling) + ":";
    for (const std::string& w : over) a6.detail += " " + w;
  }
  v.a6 = a6.ok;
  v.checks.push_back(a6);
  v.fix = v.fix || unknown;
  return v;
}

}  // namespace

OrbergResult orbergise(const std::string& latin, const std::string* original, Lang originalLang,
                       const OrbergOptions& opts0, const EngineContext& ctx) {
  OrbergResult res;
  res.text = latin;
  if (!ctx.la || !ctx.cd || !ctx.la2x || !ctx.checker || !ctx.resources) {
    res.flags.push_back("orberg-unavailable");
    return res;
  }
  try {
    Resources::Impl& R = ctx.resources->impl();
    OrbergOptions opts = opts0;
    opts.tierCeiling = std::max(1, std::min(2, opts.tierCeiling));
    opts.hasOriginal = original && !original->empty();
    TextOut rw;
    rewriteText(latin, opts, ctx, R, false, rw);
    std::string text = rw.text;
    std::vector<Change> changes = rw.changes;
    std::vector<std::pair<uint32_t, std::string>> content = rw.content;
    std::vector<std::pair<uint32_t, uint32_t>> mapped = rw.mapped;
    std::vector<std::string> flags = rw.flags;
    std::vector<rules::Reason> notes = rw.notes;
    Read rd;
    readBack(text, ctx, rd);
    Verdict vd = verdict(text, rd, opts, ctx, R);

    // ---- meaning of the rewrite (content lemmas of the input vs the output) ----
    auto meaningNow = [&](std::vector<std::string>& miss) {
      detail::LemmaSet outSet;
      for (const la2x::Sentence& s : rd.sents) detail::contentLemmas(s, R, outSet);
      miss.clear();
      const double m = meaningOf(content, mapped, outSet, miss);
      std::sort(miss.begin(), miss.end());
      miss.erase(std::unique(miss.begin(), miss.end()), miss.end());
      return m;
    };
    std::vector<std::string> missing;
    res.meaning = meaningNow(missing);

    // ---- with the original ----
    // Policy (simplify_la.tsv "rule original prefer=..."): rewrite = the Latin's own rewrite when it is clean (no Fix,
    // tier ceiling met, meaning >= 0.6, no construction left as it was), else the original's translation when that
    // one is clean; original = the original's translation first (DESIGN §10.7 read literally).
    if (opts.hasOriginal) {
      const bool preferO = R.param("original", "prefer", "rewrite") == "original";
      const bool cleanR = !vd.fix && vd.a6 && res.meaning >= 0.6 &&
                          std::find(flags.begin(), flags.end(), "structure-kept") == flags.end();
      if (!rw.changed && cleanR) {
        addFlag(flags, "orberg-kept");   // already beginner Latin: the input stays as it is
      } else if (!ctx.fromOriginal) {
        addFlag(flags, "original-unused");
      } else if (cleanR && !preferO) {
        addFlag(flags, "orberg-latin");   // the original was not needed
      } else {
        std::vector<uint32_t> prefer;
        for (uint32_t l : rw.inputLemmas)
          if (R.tier(l) <= opts.tierCeiling) prefer.push_back(l);
        std::sort(prefer.begin(), prefer.end());
        prefer.erase(std::unique(prefer.begin(), prefer.end()), prefer.end());
        OriginalLatin ol;
        bool ok = false;
        try {
          ok = ctx.fromOriginal(*original, originalLang, opts.tierCeiling, prefer, ol);
        } catch (...) {
          ok = false;
        }
        std::string why;
        if (!ok || ol.text.empty()) why = "the original could not be translated";
        else if (ol.unknown) why = "the original has words without a Latin lemma";
        if (why.empty()) {
          std::string cand = rw.macrons ? ol.text : text::display_latin(ol.text, false);
          TextOut vo;   // vocabulary above the ceiling in the original's Latin
          rewriteText(cand, opts, ctx, R, true, vo);
          Read rd2;
          readBack(vo.text, ctx, rd2);
          Verdict vd2 = verdict(vo.text, rd2, opts, ctx, R);
          if (vd2.fix) why = "the original's Latin did not pass the grammar checks";
          else if (!vd2.a6) why = "the original's Latin is above the tier ceiling too";
          if (why.empty()) {
            Read rin;
            readBack(latin, ctx, rin);
            changes = alignChanges(alignWords(rin, R), alignWords(rd2, R));
            // reference: the original's transferred lemmas
            content.clear();
            for (uint32_t l : ol.lemmas)
              if (l != kNone && l < ctx.la->lemmaCount()) {
                const lex::Lemma x = ctx.la->lemma(l);
                if (x.key == "sum") continue;
                content.emplace_back(l, std::string(x.head));
              }
            mapped = vo.mapped;
            text = vo.text;
            rd = std::move(rd2);
            vd = std::move(vd2);
            notes = vo.notes;
            flags = vo.flags;
            res.fromOriginal = true;
            addFlag(flags, "orberg-original");
            if (ol.fallback) addFlag(flags, "frame-fallback");
            res.meaning = meaningNow(missing);
          }
        }
        if (!why.empty()) {
          addFlag(flags, "original-rejected");
          notes.push_back(rules::Reason{-1, "orbergise", "rewritten from the Latin: " + why, ""});
        }
      }
    }
    res.missing = missing;
    rules::Check a7{"A7", res.meaning >= 0.6, "meaning " + pct(res.meaning)};
    if (!missing.empty()) {
      a7.detail += "; missing:";
      for (const std::string& m : missing) a7.detail += " " + m;
    }
    vd.checks.push_back(a7);
    std::stable_sort(vd.checks.begin(), vd.checks.end(),
                     [](const rules::Check& x, const rules::Check& y) { return x.id < y.id; });

    // ---- result ----
    res.text = text;
    res.tokens = vd.tokens;
    res.checks = vd.checks;
    auto tokenAt = [&](int off) {
      if (off < 0) return -1;
      for (size_t k = 0; k < res.tokens.size(); ++k)
        if (res.tokens[k].start >= off) return (int)k;
      return -1;
    };
    for (Change& c : changes) {
      c.tokenIndex = tokenAt(c.tokenIndex);
      res.reasons.push_back(rules::Reason{c.tokenIndex, "orbergise",
                                          (c.from.empty() ? std::string("(new)") : c.from) + " -> " +
                                              (c.to.empty() ? std::string("(removed)") : c.to) + " (" +
                                              (c.why.empty() ? c.reason : c.why) + ")",
                                          reasonData(c)});
    }
    res.changes = changes;
    for (rules::Reason& n : notes) res.reasons.push_back(std::move(n));
    if (res.meaning < 0.6) addFlag(flags, "meaning-low");
    if (!vd.a6) addFlag(flags, "tier-exceeded");
    res.flags = flags;
    bool check = !vd.a6 || res.meaning < 0.6;
    for (const char* f : {"agent-guess", "structure-kept", "frame-fallback", "synonym"})
      if (std::find(flags.begin(), flags.end(), f) != flags.end()) check = true;
    res.confidence = vd.fix ? Confidence::Fix : check ? Confidence::Check : Confidence::Ok;
    return res;
  } catch (const std::exception& e) {
    res = OrbergResult();
    res.text = latin;
    res.flags.push_back("orberg-error");
    res.reasons.push_back(rules::Reason{-1, "orbergise", std::string("internal error: ") + e.what(), ""});
    return res;
  } catch (...) {
    res = OrbergResult();
    res.text = latin;
    res.flags.push_back("orberg-error");
    return res;
  }
}

std::vector<rules::CueOutput> cues(const std::vector<rules::CueInput>& in, const rules::Options& opt,
                                   const rules::Context& ctx0, const EngineContext& ctx,
                                   const std::function<void(size_t)>& progress,
                                   const std::function<bool()>& cancelled) {
  std::vector<rules::CueOutput> outs;
  outs.reserve(in.size());
  (void)ctx0;
  OrbergOptions o;
  o.tierCeiling = opt.orbergTier;
  o.keepNames = opt.orbergKeepNames;
  o.simplify = opt.orbergSimplify;
  o.macrons = opt.macrons;
  for (size_t i = 0; i < in.size(); ++i) {
    if (cancelled && cancelled()) break;
    const rules::CueInput& c = in[i];
    rules::CueOutput out;
    out.index = c.index;
    out.original = c.originalText;
    try {
      std::string flat = c.sourceText;
      std::replace(flat.begin(), flat.end(), '\n', ' ');
      if (flat.find_first_not_of(" \t\r") == std::string::npos) {
        out.confidence = Confidence::Ok;
        out.score = 1.0;
        out.meaningPercent = 100;
      } else {
        const OrbergResult r = orbergise(flat, c.originalText.empty() ? nullptr : &c.originalText, c.originalLang, o, ctx);
        const cue::Layout lay = cue::layout(r.text, ctx.maxLine, ctx.maxLines);
        out.target = lay.joined;
        out.tokens = r.tokens;
        cue::relocate(out.target, out.tokens);
        out.reasons = r.reasons;
        out.flags = r.flags;
        out.checks = r.checks;
        // A8 reading speed and lines
        rules::Check a8{"A8", true, ""};
        if (c.endMs > c.startMs) {
          const double cps = (double)subs::visibleLength(r.text) / ((double)(c.endMs - c.startMs) / 1000.0);
          if (cps > ctx.cpsLimit) {
            a8.ok = false;
            a8.detail = "reading speed " + std::to_string((int)std::lround(cps)) + " cps";
            addFlag(out.flags, "cps");
          }
        }
        if (lay.overflow) {
          a8.ok = false;
          a8.detail += std::string(a8.detail.empty() ? "" : "; ") + "too many lines";
          addFlag(out.flags, "overflow");
        }
        out.checks.push_back(a8);
        std::stable_sort(out.checks.begin(), out.checks.end(),
                         [](const rules::Check& x, const rules::Check& y) { return x.id < y.id; });
        out.confidence = r.confidence;
        if (out.confidence == Confidence::Ok && !a8.ok) out.confidence = Confidence::Check;
        out.score = out.confidence == Confidence::Ok ? 1.0 : out.confidence == Confidence::Check ? 0.7 : 0.3;
        out.meaningPercent = (int)std::lround(r.meaning * 100.0);
        out.meaningMissing = r.missing;
      }
    } catch (...) {
      out.target = c.sourceText;
      out.confidence = Confidence::Check;
      out.flags.push_back("orberg-error");
    }
    outs.push_back(std::move(out));
    if (progress) progress(i + 1);
  }
  return outs;
}

}  // namespace vp::orberg
