// grc2x Translator: glue of analysis, interlinear words, the readable sentence and the engine cue interface.
#include <algorithm>
#include <cmath>

#include "grc2x/internal.h"
#include "vp/morph.h"
#include "vp/morph_grc.h"
#include "vp/realise_la.h"
#include "vp/text.h"

namespace vp::grc2x {

constexpr uint32_t kNone = lex::kNoLemma;

using namespace vp::feat;

namespace {

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

const char* posName(uint8_t p) {
  switch (p) {
    case Noun: return "noun";
    case Verb: return "verb";
    case Adj: return "adj";
    case Adv: return "adv";
    case Pron: return "pron";
    case Prep: return "prep";
    case Conj: return "conj";
    case Particle: return "particle";
    case Num: return "num";
    case Intj: return "intj";
    default: return "";
  }
}

// First item of a one-line gloss: "to go; to step" -> "go"; "a cake or loaf" -> "cake or loaf".
std::string firstItem(std::string g, bool english) {
  const size_t cut = g.find_first_of(";,(");
  if (cut != std::string::npos) g = g.substr(0, cut);
  while (!g.empty() && g.back() == ' ') g.pop_back();
  size_t a = 0;
  while (a < g.size() && g[a] == ' ') ++a;
  g.erase(0, a);
  if (english) {
    for (const char* p : {"to ", "a ", "an ", "the "})
      if (g.compare(0, std::char_traits<char>::length(p), p) == 0) g.erase(0, std::char_traits<char>::length(p));
  }
  return g;
}

}  // namespace

struct Translator::Impl : public detail::LexicalSource {
  const lex::Lexicon& lx;
  const curated::CuratedData& cd;
  const grc::GreekData& gd;
  const grc::GreekTables& gt;
  detail::Analyser analyser;
  Impl(const lex::Lexicon& l, const curated::CuratedData& c, const grc::GreekData& g, const grc::GreekTables& t)
      : lx(l), cd(c), gd(g), gt(t), analyser(l, c, g, t) {}

  detail::Lexical lexical(uint32_t lemma, uint8_t pos, bool middle, Target tg) const override {
    detail::Lexical x;
    const lex::Lemma l = lx.lemma(lemma);
    if (l.id == kNone) { x.missing = true; return x; }
    const grc::ReadableRow* r = nullptr;
    if (middle && pos == Verb) r = gt.readable(l.key, "mid");
    if (!r) r = gt.readable(l.key, posName(pos == Article ? 0 : pos));
    if (!r) r = gt.readable(l.key);
    if (r) {
      x.word = tg == Target::En ? r->en : r->es;
      if (x.word == "-") x.word.clear();
      const std::string& tags = r->tags;
      x.person = tags.find("person") != std::string::npos;
      x.animal = tags.find("animal") != std::string::npos;
      x.mass = tags.find("mass") != std::string::npos;
      x.motion = tags.find("motion") != std::string::npos;
      const size_t pl = tags.find("plural:");
      if (pl != std::string::npos && tg == Target::En) {
        size_t e = pl + 7;
        while (e < tags.size() && tags[e] != ' ') ++e;
        x.plural = tags.substr(pl + 7, e - pl - 7);
      }
      if (tg == Target::Es) x.gender = r->esGender == "f" ? F : r->esGender == "m" ? M : 0;
      return x;
    }
    if (tg == Target::En) {
      x.word = firstItem(text::lower(std::string(l.glossEn)), true);
    } else {
      x.word = firstItem(text::lower(std::string(l.glossEs)), false);
      x.pivot = (l.flags & (1u << 8)) != 0;
    }
    if (x.word.empty()) x.missing = true;
    return x;
  }

  std::string gloss(uint32_t lemma, Target tg, bool* pivot) const {
    const lex::Lemma l = lx.lemma(lemma);
    if (l.id == kNone) return std::string();
    detail::Lexical x = lexical(lemma, l.pos, false, tg);
    if (pivot) *pivot = x.pivot;
    return x.word;
  }

  void sentence(std::string_view greek, Target tg, SentenceOut& out, const std::vector<rules::GlossaryEntry>* glossary) {
    out = SentenceOut{};
    out.greek = std::string(greek);
    analyser.analyse(greek, out.analysis, glossary);
    const Sentence& s = out.analysis;
    std::vector<detail::TokInfo> ti;
    detail::tokInfos(lx, s, ti);
    detail::Side side;
    detail::Built b;
    detail::buildFrames(lx, s, ti, *this, tg, side, b);
    out.text = tg == Target::En ? detail::realiseEnglish(b, side) : detail::realiseSpanish(b, side);
    for (const std::string& f : b.flags) out.flags.push_back(f);
    for (size_t i = 0; i < b.frames.size(); ++i) {
      if (i) out.frame += " | ";
      out.frame += frame::describe(b.frames[i]);
    }
    double conf = 0;
    size_t nw = 0;
    for (size_t i = 0; i < s.tokens.size(); ++i) {
      const Token& t = s.tokens[i];
      if (t.kind != TokKind::Word) continue;
      Word w;
      w.token = (int)i;
      w.text = t.text;
      w.start = t.start;
      w.end = t.end;
      w.unknown = t.unknown;
      w.nameGuess = t.nameGuess;
      w.accentDiffers = t.accentDiffers;
      w.fromRule = t.fromRule;
      w.confidence = t.confidence;
      w.role = i < b.roles.size() ? b.roles[i] : std::string();
      if (const Reading* r = t.best()) {
        const Features f = unpack(r->packed);
        w.lemma = r->lemma;
        w.name = r->name;
        w.features = realise::featureView(r->packed);
        if (r->lemma != kNone) {
          const lex::Lemma l = lx.lemma(r->lemma);
          w.head = grc::display(l.head);
          w.featureText = detail::featureText(f, r->closed ? f.pos : l.pos);
          uint8_t tier = l.tier;
          if (const curated::TierEntry* te = cd.tierGreek(l.key))
            if (te->tier && (!tier || te->tier < tier)) tier = te->tier;
          w.tier = tier;
          if (const curated::EmojiEntry* e = cd.emojiGreek(l.key))
            if (l.pos == Noun) w.emoji = e->emoji;
          const bool middle = f.voice == Middle || f.voice == Passive;
          const detail::Lexical x = lexical(r->lemma, r->closed ? f.pos : l.pos, middle, tg);
          w.gloss = x.word;
          w.glossPivot = x.pivot;
        } else {
          w.head = r->nameEn.empty() ? t.text : r->nameEn;
          w.featureText = detail::featureText(f, Name);
          w.gloss = r->nameEn;
        }
        for (size_t k = 1; k < t.readings.size() && w.alternatives.size() < 4; ++k) {
          const Reading& a = t.readings[k];
          if (a.prior < r->prior - 0.5) break;
          const Features af = unpack(a.packed);
          std::string head = a.lemma != kNone ? grc::display(lx.lemma(a.lemma).head) : a.nameEn;
          w.alternatives.push_back(t.text + " (" + head + ", " +
                                   detail::featureText(af, a.closed ? af.pos : a.lemma != kNone ? lx.lemma(a.lemma).pos : (uint8_t)Name) + ")");
        }
      }
      if (w.unknown) out.flags.push_back("unknown");
      if (w.nameGuess) out.flags.push_back("name-guessed");
      if (w.accentDiffers) out.flags.push_back("accent-differs");
      if (w.confidence < 0.6) out.flags.push_back("ambiguous");
      conf += w.confidence;
      ++nw;
      out.words.push_back(std::move(w));
    }
    std::sort(out.flags.begin(), out.flags.end());
    out.flags.erase(std::unique(out.flags.begin(), out.flags.end()), out.flags.end());
    out.confidence = nw ? conf / (double)nw : 0;
  }
};

Translator::Translator(const lex::Lexicon& grc, const curated::CuratedData& cd, const grc::GreekData& gd,
                       const grc::GreekTables& gt)
    : impl_(std::make_unique<Impl>(grc, cd, gd, gt)) {}
Translator::~Translator() = default;

void Translator::analyse(std::string_view greek, Sentence& out, const std::vector<rules::GlossaryEntry>* glossary) const {
  impl_->analyser.analyse(greek, out, glossary);
}

void Translator::sentence(std::string_view greek, Target t, SentenceOut& out,
                          const std::vector<rules::GlossaryEntry>* glossary) {
  impl_->sentence(greek, t, out, glossary);
}

void Translator::resetDiscourse() {}

std::string Translator::gloss(uint32_t lemma, Target t, bool* pivot) const { return impl_->gloss(lemma, t, pivot); }

std::vector<std::pair<size_t, size_t>> splitSentences(std::string_view text) {
  std::vector<std::pair<size_t, size_t>> out;
  size_t a = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    bool end = c == '.' || c == ';' || c == '!' || c == '?';
    if (!end && (unsigned char)c == 0xCD && i + 1 < text.size() && (unsigned char)text[i + 1] == 0xBE) { end = true; ++i; }
    if (!end) continue;
    size_t e = i + 1;
    while (e < text.size() && (text[e] == '.' || text[e] == '!' || text[e] == '?' || text[e] == '"' || text[e] == ')')) ++e;
    size_t s0 = a;
    while (s0 < e && text[s0] == ' ') ++s0;
    if (e > s0) out.emplace_back(s0, e);
    a = e;
    i = e - 1;
  }
  size_t s0 = a;
  while (s0 < text.size() && text[s0] == ' ') ++s0;
  if (s0 < text.size()) out.emplace_back(s0, text.size());
  return out;
}

std::vector<rules::CueOutput> Translator::cues(const std::vector<rules::CueInput>& cues, const rules::Options& opt,
                                               const rules::Context& ctx, const std::function<void(size_t)>& progress,
                                               const std::function<bool()>& cancelled) {
  std::vector<rules::CueOutput> outs;
  outs.reserve(cues.size());
  const Target t = opt.target == rules::Lang::Es ? Target::Es : Target::En;
  for (size_t ci = 0; ci < cues.size(); ++ci) {
    if (cancelled && cancelled()) break;
    const rules::CueInput& in = cues[ci];
    rules::CueOutput o;
    o.index = in.index;
    try {
      const auto ranges = splitSentences(in.sourceText);
      double conf = 0;
      size_t ns = 0;
      bool unknown = false, ambiguous = false, check = false, accent = false;
      std::vector<std::string> unknownWords, ambiguousWords, accentWords;
      std::string wordByWord;
      for (const auto& r : ranges) {
        SentenceOut so;
        impl_->sentence(std::string_view(in.sourceText).substr(r.first, r.second - r.first), t, so, &ctx.glossary);
        if (!o.target.empty() && !so.text.empty()) o.target += ' ';
        o.target += so.text;
        conf += so.confidence;
        ++ns;
        for (const std::string& f : so.flags)
          if (std::find(o.flags.begin(), o.flags.end(), f) == o.flags.end()) o.flags.push_back(f);
        for (const Word& w : so.words) {
          rules::TokenView tv;
          tv.text = tv.display = w.text;
          tv.start = (int)r.first + w.start;
          tv.end = (int)r.first + w.end;
          tv.lemmaId = w.lemma;
          tv.hasLemma = w.lemma != lex::kNoLemma;
          tv.features = w.features;
          tv.tier = w.tier;
          if (opt.emoji) tv.emoji = w.emoji;
          tv.unknown = w.unknown;
          tv.fromRule = w.fromRule;
          const int ti = (int)o.tokens.size();
          o.tokens.push_back(tv);
          std::string data = "{\"lemmaId\":" + (tv.hasLemma ? std::to_string(w.lemma) : std::string("null")) +
                             ",\"head\":\"" + jsonEscape(w.head) + "\",\"gloss\":\"" + jsonEscape(w.gloss) +
                             "\",\"glossLang\":\"" + (t == Target::En ? "en" : "es") + "\",\"pivot\":" +
                             (w.glossPivot ? "true" : "false") + ",\"form\":\"" + jsonEscape(w.featureText) +
                             "\",\"role\":\"" + jsonEscape(w.role) + "\",\"confidence\":" +
                             std::to_string(w.confidence).substr(0, 5) + ",\"accentDiffers\":" +
                             (w.accentDiffers ? "true" : "false") + ",\"alternatives\":[";
          for (size_t k = 0; k < w.alternatives.size(); ++k)
            data += (k ? ",\"" : "\"") + jsonEscape(w.alternatives[k]) + "\"";
          data += "],\"why\":[";
          const Token& tk = so.analysis.tokens[(size_t)w.token];
          for (size_t k = 0; k < tk.why.size(); ++k) data += (k ? ",\"" : "\"") + jsonEscape(tk.why[k]) + "\"";
          data += "]}";
          std::string txt = w.head.empty() ? w.text : w.head;
          if (!w.featureText.empty()) txt += " (" + w.featureText + ")";
          if (!w.gloss.empty()) txt += ": " + w.gloss + (w.glossPivot ? (t == Target::Es ? " (vía inglés)" : " (via English)") : "");
          o.reasons.push_back(rules::Reason{ti, "analysis", txt, data});
          if (w.unknown) { unknown = true; unknownWords.push_back(w.text); }
          if (w.confidence < 0.6) { ambiguous = true; ambiguousWords.push_back(w.text); }
          if (w.accentDiffers) { accent = true; accentWords.push_back(w.text); }
          if (w.nameGuess) check = true;
          std::string g = w.gloss.empty() ? w.text : w.gloss;
          if (!wordByWord.empty()) wordByWord += ' ';
          wordByWord += g;
        }
      }
      if (std::find(o.flags.begin(), o.flags.end(), "source-tokens") == o.flags.end()) o.flags.push_back("source-tokens");
      rules::Check a1{"A1", !unknown, unknown ? "unknown Greek words:" : ""};
      for (const std::string& w : unknownWords) a1.detail += " " + w;
      if (!unknown && accent) {   // accent-insensitive readings only: a warning (Check), never Fix
        a1.detail = "warning: accent differs:";
        for (const std::string& w : accentWords) a1.detail += " " + w;
      }
      rules::Check amb{"ambiguity", !ambiguous, ambiguous ? "several readings fit:" : ""};
      for (const std::string& w : ambiguousWords) amb.detail += " " + w;
      o.checks.push_back(a1);
      o.checks.push_back(amb);
      const double mean = ns ? conf / (double)ns : 1.0;
      for (const char* f : {"name-guessed", "no-verb", "accent-differs"})
        if (std::find(o.flags.begin(), o.flags.end(), f) != o.flags.end()) check = true;
      o.confidence = unknown ? rules::Confidence::Fix
                             : (ambiguous || check || mean < 0.7) ? rules::Confidence::Check : rules::Confidence::Ok;
      o.score = std::round(mean * 1000) / 1000;
      if (!wordByWord.empty()) o.alternatives.push_back(rules::Alternative{wordByWord, "word by word", 0.5});
    } catch (const std::exception& e) {
      o.target = in.sourceText;
      o.confidence = rules::Confidence::Fix;
      o.reasons.push_back(rules::Reason{-1, "analysis", std::string("could not analyse: ") + e.what(), ""});
    } catch (...) {
      o.target = in.sourceText;
      o.confidence = rules::Confidence::Fix;
    }
    outs.push_back(std::move(o));
    if (progress) progress(outs.size());
  }
  return outs;
}

}  // namespace vp::grc2x
