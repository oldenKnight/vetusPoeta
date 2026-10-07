// orbergise() and cues() (vp/orberg.h): the rewrite of a cue sentence by sentence, the never-nonsense gate (a rewrite
// that adds a grammar fault or misreads a swapped word is discarded), the meaning check (la2x re-analysis, gloss
// comparison of substitutions, the original-language cue as evidence), the checks (A1-A4 from the Latin checker, A6
// with participles counted as their verbs, A7 = meaning) and the confidence. The original never replaces the Latin.
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
  std::vector<detail::MapEntry> mapped;
  std::vector<uint32_t> inputLemmas;
  std::vector<std::string> kept;
  bool changed = false, macrons = false, unconfirmed = false;
};

void addFlag(std::vector<std::string>& f, const std::string& x) {
  if (std::find(f.begin(), f.end(), x) == f.end()) f.push_back(x);
}

// Never nonsense (C27): a rewritten sentence must not add an A1/A3/A4 fault the input did not have, every swapped word
// must be read back as the word it was meant to be, and no unknown word may appear. `why` names the first failure.
bool sound(const std::string& input, const detail::SentenceOut& so, const OrbergOptions& o, const EngineContext& ctx,
           Resources::Impl& R, std::string& why) {
  check::Options co;
  co.tierCeiling = (uint8_t)o.tierCeiling;
  co.glossary = ctx.glossary;
  check::Report ri, ro;
  ctx.checker->check(input, co, ri);
  ctx.checker->check(so.text, co, ro);
  auto ok = [](const check::Report& r, const char* id, std::string* detail) {
    for (const rules::Check& c : r.checks)
      if (c.id == id && !c.ok) {
        if (detail) *detail = c.detail;
        return false;
      }
    return true;
  };
  for (const char* id : {"A1", "A3", "A4"}) {
    std::string d;
    if (!ok(ro, id, &d) && ok(ri, id, nullptr)) {
      why = std::string(id) + " fails on the rewrite" + (d.empty() ? std::string() : " (" + d + ")");
      return false;
    }
  }
  la2x::Sentence s;
  ctx.la2x->analyser().analyseWords(so.text, s, ctx.glossary);
  detail::LemmaSet best;
  detail::bestContentLemmas(s, R, best);
  std::vector<std::string> bestKeys;
  for (uint32_t l : best.lemmas) bestKeys.push_back(std::string(R.la.lemma(l).key));
  for (const detail::MapEntry& m : so.mapped) {
    if (m.kind == detail::MapKind::Reading || m.kind == detail::MapKind::Structure) continue;
    if (m.to == kNone || best.has(m.to)) continue;
    const std::string k(R.la.lemma(m.to).key);
    if (std::find(bestKeys.begin(), bestKeys.end(), k) != bestKeys.end()) continue;
    // a fixed form (pair rows: aliquid) written as the lemma's own head and read with it among its readings
    bool written = false;
    for (const la2x::Token& t : s.tokens)
      if (t.kind == la2x::TokKind::Word && text::latin_key(t.text) == k)
        for (const la2x::Reading& r : t.readings) written = written || r.lemma == m.to;
    if (written) continue;
    why = "the new word " + std::string(R.la.lemma(m.to).head) + " would be read as another word";
    return false;
  }
  const std::string inKey = " " + text::latin_key(input) + " ";
  for (const la2x::Token& t : s.tokens)
    if (t.kind == la2x::TokKind::Word && t.unknown && !t.nameGuess &&
        inKey.find(" " + text::latin_key(t.text) + " ") == std::string::npos) {
      why = "unknown word " + t.text;
      return false;
    }
  return true;
}

// The subject person / number an original-language cue names (evidence for a gerund of obligation without an agent):
// the first subject pronoun (English, Spanish) or a form of deber / tener que. 0 when none.
std::pair<uint8_t, uint8_t> originalPerson(const std::vector<std::string>& w, Lang lang) {
  static const std::pair<const char*, std::pair<uint8_t, uint8_t>> en[] = {
      {"i", {P1, Sg}}, {"we", {P1, Pl}}, {"you", {P2, Sg}}, {"he", {P3, Sg}}, {"she", {P3, Sg}}, {"they", {P3, Pl}}};
  static const std::pair<const char*, std::pair<uint8_t, uint8_t>> es[] = {
      {"yo", {P1, Sg}}, {"nosotros", {P1, Pl}}, {"nosotras", {P1, Pl}}, {"tú", {P2, Sg}}, {"ustedes", {P2, Pl}},
      {"él", {P3, Sg}}, {"ella", {P3, Sg}}, {"ellos", {P3, Pl}}, {"ellas", {P3, Pl}}, {"debo", {P1, Sg}},
      {"debemos", {P1, Pl}}, {"debes", {P2, Sg}}, {"deben", {P3, Pl}}, {"tengo", {P1, Sg}}, {"tenemos", {P1, Pl}},
      {"tienes", {P2, Sg}}, {"tienen", {P3, Pl}}};
  for (const std::string& x : w) {
    if (lang == Lang::Es) {
      for (const auto& p : es)
        if (x == p.first) return p.second;
    } else {
      for (const auto& p : en)
        if (x == p.first) return p.second;
    }
  }
  return {0, 0};
}

void rewriteText(const std::string& latin, const OrbergOptions& o, const EngineContext& ctx, Resources::Impl& R,
                 bool vocabOnly, TextOut& out, const std::vector<std::string>* evidence = nullptr,
                 std::pair<uint8_t, uint8_t> person = {0, 0}) {
  const auto ranges = la2x::splitSentences(latin);
  size_t prev = 0;
  for (const auto& r : ranges) {
    if (r.first > prev) out.text += latin.substr(prev, r.first - prev);
    detail::SentenceIn in;
    in.text = latin.substr(r.first, r.second - r.first);
    in.opt = &o;
    in.ctx = &ctx;
    in.simplifyOnly = vocabOnly;
    in.evidence = evidence;
    if (ranges.size() == 1) {   // the person of a one-sentence cue only (no guess across sentences)
      in.originalPerson = person.first;
      in.originalNumber = person.second;
    }
    detail::SentenceOut so;
    detail::rewriteSentence(in, R, so);
    if (so.changed) {
      std::string why;
      if (!sound(in.text, so, o, ctx, R, why)) {
        // fall back to the vocabulary alone, then to the structure alone, then to the sentence as written
        bool used = false;
        for (int variant = 0; variant < 2 && !used; ++variant) {
          detail::SentenceIn v = in;
          if (variant == 0) v.simplifyOnly = true;
          else v.noVocab = true;
          if (variant == 1 && vocabOnly) break;
          detail::SentenceOut vo;
          detail::rewriteSentence(v, R, vo);
          std::string why2;
          if (vo.changed && sound(in.text, vo, o, ctx, R, why2)) {
            vo.notes.push_back(rules::Reason{-1, "orbergise", "part of the rewrite discarded: " + why, ""});
            vo.flags.push_back("rewrite-partial");
            so = std::move(vo);
            used = true;
          }
        }
        if (!used) {
          detail::SentenceOut keep;
          keep.text = in.text;
          keep.content = so.content;
          keep.inputLemmas = so.inputLemmas;
          keep.macrons = so.macrons;
          keep.notes.push_back(rules::Reason{-1, "orbergise", "rewrite discarded: " + why, ""});
          keep.flags.push_back("rewrite-discarded");
          so = std::move(keep);
        }
      }
    }
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
    for (const std::string& k : so.kept) out.kept.push_back(k);
    out.changed = out.changed || so.changed;
    out.macrons = out.macrons || so.macrons;
    out.unconfirmed = out.unconfirmed || so.unconfirmed;
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
    ctx.la2x->analyser().analyseWords(std::string_view(t).substr(r.first, r.second - r.first), out.sents.back(),
                                      ctx.glossary);
    out.offsets.push_back(r.first);
  }
}

bool isName(const la2x::Token& t, const lex::Lexicon& la) {
  for (const la2x::Reading& r : t.readings)
    if (r.name || (r.lemma != kNone && (la.lemma(r.lemma).flags & lex::ProperName))) return true;
  return t.nameGuess;
}

// The meaning check (C27): every content lemma of the input (best readings) must be read in the output, or be replaced
// by the same written word read otherwise, a rule's regeneration, a teacher-edited row, or a word whose glosses (la2x
// curated glosses and the lexicon's first sense, English and Spanish) share its sense. Losses are listed as
// "head -> new head" (or the head alone when nothing replaced it).
double meaningOf(const std::vector<std::pair<uint32_t, std::string>>& content0,
                 const std::vector<detail::MapEntry>& mapped,
                 const detail::LemmaSet& outSet, Resources::Impl& R, const la2x::Translator* tr,
                 std::vector<std::string>& missing, size_t& total) {
  std::vector<std::pair<uint32_t, std::string>> content = content0;
  std::sort(content.begin(), content.end());
  content.erase(std::unique(content.begin(), content.end(),
                            [](const std::pair<uint32_t, std::string>& x, const std::pair<uint32_t, std::string>& y) {
                              return x.first == y.first;
                            }),
                content.end());
  total = content.size();
  if (content.empty()) return 1.0;
  size_t ok = 0;
  for (const auto& c : content) {
    bool found = false;
    if (c.first & 0x80000000u) found = outSet.hasName(text::latin_key(c.second));
    else found = outSet.has(c.first);
    std::string lostTo;
    std::vector<uint32_t> frontier{c.first};
    for (int step = 0; step < 3 && !found && !frontier.empty(); ++step) {
      std::vector<uint32_t> next;
      for (uint32_t f : frontier)
        for (const detail::MapEntry& m : mapped) {
          if (m.from != f || m.to == f || m.to == kNone) continue;
          const bool same = m.kind == detail::MapKind::Reading || m.kind == detail::MapKind::Structure ||
                            m.kind == detail::MapKind::Phrase || R.glossSame(f, m.to, tr);
          if (!same) {
            if (lostTo.empty() && m.to < R.la.lemmaCount()) lostTo = std::string(R.la.lemma(m.to).head);
            continue;
          }
          if (outSet.has(m.to)) found = true;
          next.push_back(m.to);
        }
      frontier.swap(next);
    }
    if (found) ++ok;
    else missing.push_back(lostTo.empty() ? c.second : c.second + " -> " + lostTo);
  }
  return (double)ok / (double)content.size();
}

// Lower-case words of an original-language cue (sense evidence and the meaning check).
std::vector<std::string> originalWords(const std::string& t) {
  std::vector<std::string> out;
  std::string w;
  auto flush = [&]() {
    if (w.size() >= 2 && std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
    w.clear();
  };
  size_t i = 0;
  while (i < t.size()) {
    const size_t a = i;
    const char32_t c = text::decodeUtf8(t, i);
    const bool letter =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7);
    if (letter || (c == '\'' && !w.empty())) w += text::lower(t.substr(a, i - a));
    else flush();
  }
  flush();
  for (std::string& x : out)
    if (!x.empty() && x.back() == '\'') x.pop_back();
  return out;
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

// The document's macron convention (C27): a length mark anywhere in the cues means a text written with macrons (1);
// none means a text without them (0): new words get none either.
int macronConvention(const std::vector<rules::CueInput>& in) {
  for (const rules::CueInput& c : in)
    if (text::nfd(c.sourceText).find("\xCC\x84") != std::string::npos) return 1;
  return 0;
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
    // the original-language cue is evidence (C27): its words rank same-sense candidates (English originals) and enter
    // the meaning check; it never replaces the Latin
    std::vector<std::string> evidence;
    if (opts.hasOriginal) evidence = originalWords(*original);
    TextOut rw;
    rewriteText(latin, opts, ctx, R, false, rw,
                opts.hasOriginal && originalLang == Lang::En && !evidence.empty() ? &evidence : nullptr,
                opts.hasOriginal ? originalPerson(evidence, originalLang) : std::pair<uint8_t, uint8_t>{0, 0});
    const std::string& text = rw.text;
    std::vector<std::string> flags = rw.flags;
    Read rd;
    readBack(text, ctx, rd);
    Verdict vd = verdict(text, rd, opts, ctx, R);

    // ---- meaning: the content lemmas a reader reads in the output (la2x best readings) against the input's ----
    detail::LemmaSet outSet;
    for (const la2x::Sentence& s : rd.sents) detail::bestContentLemmas(s, R, outSet);
    std::vector<std::string> missing;
    size_t total = 0;
    double meaning = meaningOf(rw.content, rw.mapped, outSet, R, ctx.la2x, missing, total);
    if (opts.hasOriginal) {
      addFlag(flags, "original-evidence");
      addFlag(flags, rw.changed ? "orberg-latin" : "orberg-kept");
      // words of the original the input covers (through la2x glosses) must stay covered by the output
      if (rw.changed && text != latin) {
        const la2x::Target tl = originalLang == Lang::Es ? la2x::Target::Es : la2x::Target::En;
        size_t lost = 0;
        for (const std::string& w : evidence) {
          // the word or a simple base form of it (hurries: hurry; went is left as it is) that the input covers
          std::vector<std::string> forms{w};
          if (tl == la2x::Target::En) {
            auto cut = [&](const char* suf, const char* add) {
              const size_t n = std::char_traits<char>::length(suf);
              if (w.size() > n + 2 && w.compare(w.size() - n, n, suf) == 0)
                forms.push_back(w.substr(0, w.size() - n) + add);
            };
            cut("ies", "y");
            cut("es", "");
            cut("s", "");
            cut("ed", "");
            cut("ed", "e");
            cut("ing", "");
            cut("ing", "e");
          }
          for (const std::string& f : forms) {
            const std::vector<std::string> one{f};
            if (ctx.la2x->roundTripOverlap(latin, one, tl) < 1.0) continue;
            if (ctx.la2x->roundTripOverlap(text, one, tl) < 1.0) {
              ++lost;
              missing.push_back("\"" + w + "\" (original)");
            }
            break;
          }
        }
        if (lost) meaning = (meaning * (double)total) / (double)(total + lost);
      }
    }
    std::sort(missing.begin(), missing.end());
    missing.erase(std::unique(missing.begin(), missing.end()), missing.end());
    res.meaning = meaning;
    res.missing = missing;
    rules::Check a7{"A7", res.meaning >= 0.6 && missing.empty(), "meaning " + pct(res.meaning)};
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
    for (Change c : rw.changes) {
      c.tokenIndex = tokenAt(c.tokenIndex);
      res.reasons.push_back(rules::Reason{c.tokenIndex, "orbergise",
                                          (c.from.empty() ? std::string("(new)") : c.from) + " -> " +
                                              (c.to.empty() ? std::string("(removed)") : c.to) + " (" +
                                              (c.why.empty() ? c.reason : c.why) + ")",
                                          reasonData(c)});
      res.changes.push_back(std::move(c));
    }
    for (const rules::Reason& n : rw.notes) {
      bool dup = false;   // the same note from two sentences of one cue ("Serus kept ...") once
      for (const rules::Reason& x : res.reasons) dup = dup || (x.kind == n.kind && x.text == n.text);
      if (!dup) res.reasons.push_back(n);
    }
    if (rw.unconfirmed) addFlag(flags, "synonym");   // a swap that is not a confirmed pair: the teacher checks it
    if (!rw.kept.empty()) addFlag(flags, "tier-kept");
    if (res.meaning < 0.6) addFlag(flags, "meaning-low");
    if (!missing.empty()) addFlag(flags, "meaning-lost");
    if (!vd.a6) addFlag(flags, "tier-exceeded");
    res.flags = flags;
    bool check = !vd.a6 || res.meaning < 0.6 || !missing.empty();
    for (const char* f : {"agent-guess", "structure-kept", "frame-fallback", "synonym", "tier-kept", "rewrite-partial",
                          "rewrite-discarded"})
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
  o.sourceMacrons = macronConvention(in);
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
        // an unchanged cue keeps its own lines byte for byte (C27); a rewritten one is laid out again
        out.target = r.text == flat ? c.sourceText : lay.joined;
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
