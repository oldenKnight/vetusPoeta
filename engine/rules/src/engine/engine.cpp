// makeEngine(): vp::rules::Engine end to end (DESIGN.md §9, §10): cues -> sentences (frame/sentences) -> SemFrames
// (frame/) -> LaClause (transfer/) -> Latin text (C1 realise_la) -> checks A1-A4/A6 (C1 check) + A5/A7/A8/A9 here ->
// confidence (§10.4) -> pieces back on the cues (cue/). Every public entry point catches at the boundary.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#include "vp/check.h"
#include "vp/cue.h"
#include "vp/curated.h"
#include "vp/engine_config.h"
#include "vp/frame.h"
#include "engine_grc/engine_grc.h"   // C12 grc hook
#include "vp/la2x.h"   // C11 la2x hook
#include "vp/orberg.h"   // C14 orberg hook
#include "vp/lex.h"
#include "vp/morph.h"
#include "vp/nlp.h"
#include "vp/realise_la.h"
#include "vp/rules.h"
#include "vp/subs.h"
#include "vp/text.h"
#include "vp/transfer.h"

namespace vp::rules {

namespace stdfs = std::filesystem;

EngineConfig defaultEngineConfig() {
  EngineConfig c;
  auto env = [](const char* n) -> std::string {
    const char* v = std::getenv(n);
    return v && *v ? std::string(v) : std::string();
  };
  c.dataDir = env("VP_DATA_WORK");
  if (c.dataDir.empty()) c.dataDir = "data/work";
  c.curatedDir = env("VP_CURATED_DIR");
  if (c.curatedDir.empty()) c.curatedDir = "data/curated";
  c.nlpDir = env("VP_NLP_DIR");
  if (c.nlpDir.empty()) c.nlpDir = (stdfs::path(c.dataDir) / "nlp").string();
  return c;
}

namespace {

using frame::Kind;
using frame::SemSentence;
using frame::SourceSentence;

const char* kModelHint = "The language analysis files (english/spanish .tag.vpt and .dep.vpt) are missing from the "
                         "data folder. Reinstall the app or point VP_NLP_DIR at them.";

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

std::string fmt(double v) {
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(3);
  os << v;
  return os.str();
}

// First code point to lower case (ASCII and the macron capitals).
void decapitalise(std::string& t) {
  if (t.empty()) return;
  size_t i = 0;
  const char32_t c = text::decodeUtf8(t, i);
  char32_t l = c;
  if (c >= U'A' && c <= U'Z') l = c + 32;
  else if (c == 0x0100 || c == 0x0112 || c == 0x012A || c == 0x014C || c == 0x016A || c == 0x0232) l = c + 1;
  else if (c >= 0xC0 && c <= 0xDE && c != 0xD7) l = c + 32;
  if (l == c) return;
  std::string lo;
  text::appendUtf8(lo, l);
  t.replace(0, i, lo);
}
void capitalise(std::string& t) { realise::Punctuation::capitaliseFirst(t); }

bool endsWithAny(const std::string& s, const char* chars) {
  return !s.empty() && std::string(chars).find(s.back()) != std::string::npos;
}

// Rebuilds `text` with new token strings (same order); gaps between tokens are kept.
void rewriteTokens(cue::Latin& l, const std::vector<std::string>& texts) {
  std::string out;
  int cursor = 0;
  for (size_t i = 0; i < l.tokens.size(); ++i) {
    rules::TokenView& t = l.tokens[i];
    if (t.start < cursor || t.end > (int)l.text.size() || t.start > t.end) continue;
    out += l.text.substr((size_t)cursor, (size_t)(t.start - cursor));
    const int ns = (int)out.size();
    out += texts[i];
    cursor = t.end;
    t.start = ns;
    t.end = (int)out.size();
    t.text = texts[i];
  }
  out += l.text.substr((size_t)std::min<int>(cursor, (int)l.text.size()));
  l.text = std::move(out);
}

// What one sentence produced.
struct SentOut {
  cue::Latin latin;
  std::vector<int> srcOffset;           // per Latin token: byte offset of the source word it translates (-1)
  std::vector<Reason> reasons;          // tokenIndex relative to latin.tokens (-1: the sentence)
  std::vector<std::string> flags;
  std::vector<transfer::Choice> choices;
  std::vector<std::string> unknown;
  std::vector<std::string> missing;     // A7: source content words not accounted for
  double minMargin = 1.0;
  bool nonverbal = false, song = false, copied = false;
  std::vector<Alternative> alternatives;   // whole-sentence alternatives
  // C24: the gender of the person addressed by name or title in this sentence (feat::M / F, 0 = none) and whether the
  // sentence asks for an answer (a question, an order, or an address alone): the next cue's speaker is then that person
  uint8_t vocGender = 0;
  bool invites = false;
};

void addFlag(std::vector<std::string>& f, const std::string& x) {
  if (std::find(f.begin(), f.end(), x) == f.end()) f.push_back(x);
}

class RulesEngine final : public Engine {
 public:
  explicit RulesEngine(EngineConfig cfg) : cfg_(std::move(cfg)) {}

  Result<void> setLexicons(const lex::Lexicon* latin, const lex::Lexicon* greek, const lex::Lexicon* english,
                           const lex::Lexicon* spanish) override {
    std::lock_guard<std::mutex> lk(m_);
    la_ = latin;
    grc_ = greek;
    en_ = english;
    es_ = spanish;
    realiser_.reset();
    checker_.reset();
    xfer_.reset();
    fbEn_.reset();
    fbEs_.reset();
    return Result<void>();
  }

  Result<std::vector<CueOutput>> translate(const std::vector<CueInput>& cues, const Options& opt, const Context& ctx,
                                           const std::function<void(size_t)>& progress,
                                           const std::function<bool()>& cancelled) override {
    std::lock_guard<std::mutex> lk(m_);
    try {
      if (grcPair(opt)) return grcTranslate(cues, opt, ctx, progress, cancelled);   // C12 grc hook
      if (orbergPair(opt)) return orbergTranslate(cues, opt, ctx, progress, cancelled);   // C14 orberg hook
      if (la2xPair(opt)) return la2xTranslate(cues, opt, ctx, progress, cancelled);   // C11 la2x hook
      return translateImpl(cues, opt, ctx, progress, cancelled);
    } catch (const std::exception& e) {
      return Result<std::vector<CueOutput>>(ErrorCode::Internal, std::string("rules translate: ") + e.what(),
                                            "The translation stopped because of an internal error. Your project is safe.");
    } catch (...) {
      return Result<std::vector<CueOutput>>(ErrorCode::Internal, "rules translate: unknown exception",
                                            "The translation stopped because of an internal error. Your project is safe.");
    }
  }

  Result<CueOutput> check(const CueInput& cue, const std::string& target, const Options& opt, const Context& ctx) override {
    std::lock_guard<std::mutex> lk(m_);
    try {
      if (grcPair(opt)) return grcCheck(cue, target, opt, ctx);   // C12 grc hook
      Result<void> r = ensureLatin();
      if (!r.ok()) return Result<CueOutput>(r.error());
      return Result<CueOutput>(checkImpl(cue, target, opt, ctx));
    } catch (const std::exception& e) {
      return Result<CueOutput>(ErrorCode::Internal, std::string("rules check: ") + e.what(), "The cue could not be checked.");
    }
  }

  Result<InspectResult> inspect(const std::string& word, Lang lang, const Options& opt) override {
    std::lock_guard<std::mutex> lk(m_);
    try {
      if (grcInspects(lang, opt)) return grcInspect(word, lang, opt);   // C12 grc hook
      return la2xInspect(inspectImpl(word, lang, opt), lang, opt);   // C11 la2x hook
    } catch (const std::exception& e) {
      return Result<InspectResult>(ErrorCode::Internal, std::string("rules inspect: ") + e.what(),
                                   "The word could not be looked up.");
    }
  }

  std::string version() const override { return "rules-1"; }

 private:
  // ---- initialisation --------------------------------------------------------------------------------------------
  Result<void> ensureCurated() {
    if (cd_) return Result<void>();
    std::vector<stdfs::path> tries = {cfg_.curatedDir, stdfs::path(cfg_.dataDir) / "curated",
                                      stdfs::path(cfg_.dataDir) / ".." / "curated",
                                      stdfs::path(cfg_.dataDir) / ".." / ".." / "data" / "curated"};
    Error last{ErrorCode::NotFound, "curated tables not found", "The rule tables (data/curated) are missing."};
    for (const stdfs::path& p : tries) {
      std::error_code ec;
      if (p.empty() || !stdfs::is_directory(p, ec)) continue;
      Result<curated::CuratedData> r = curated::CuratedData::load(p);
      if (!r.ok()) { last = r.error(); continue; }
      cd_ = std::make_unique<curated::CuratedData>(std::move(r.value()));
      return Result<void>();
    }
    return Result<void>(last);
  }

  Result<void> ensureLatin() {
    if (!la_)
      return Result<void>(ErrorCode::LexiconMissing, "no Latin lexicon",
                          "The Latin dictionary (latin.vpl) is not installed.");
    Result<void> c = ensureCurated();
    if (!c.ok()) return c;
    if (!realiser_) realiser_ = std::make_unique<realise::LatinRealiser>(*la_, *cd_);
    if (!checker_) checker_ = std::make_unique<check::LatinChecker>(*la_, *cd_);
    if (!xfer_) xfer_ = std::make_unique<transfer::Transfer>(*la_, *cd_);
    return Result<void>();
  }

  Result<const frame::FrameBuilder*> builder(frame::SrcLang lang) {
    std::unique_ptr<nlp::Pipeline>& pipe = lang == frame::SrcLang::En ? nlpEn_ : nlpEs_;
    std::unique_ptr<frame::FrameBuilder>& fb = lang == frame::SrcLang::En ? fbEn_ : fbEs_;
    if (!pipe) {
      const std::string base = lang == frame::SrcLang::En ? "english" : "spanish";
      const stdfs::path dir = cfg_.nlpDir;
      Result<nlp::Pipeline> r = nlp::Pipeline::open(lang == frame::SrcLang::En ? nlp::Lang::En : nlp::Lang::Es,
                                                    (dir / (base + ".tag.vpt")).string(),
                                                    (dir / (base + ".dep.vpt")).string());
      if (!r.ok())
        return Result<const frame::FrameBuilder*>(ErrorCode::LexiconMissing, "nlp models: " + r.error().message,
                                                  kModelHint);
      pipe = std::make_unique<nlp::Pipeline>(std::move(r.value()));
    }
    if (!fb) fb = std::make_unique<frame::FrameBuilder>(lang, pipe.get(), lang == frame::SrcLang::En ? en_ : es_, *cd_);
    return Result<const frame::FrameBuilder*>(fb.get());
  }

  // ---- display ------------------------------------------------------------------------------------------------------
  // The macrons option on every token (overrides and tie bars are applied by realise::Macrons when the form is made).
  void display(cue::Latin& l, bool macrons) const {
    std::vector<std::string> texts;
    texts.reserve(l.tokens.size());
    for (rules::TokenView& t : l.tokens) {
      const std::string d = t.display.empty() ? t.text : t.display;
      t.display = d;
      texts.push_back(macrons ? d : text::display_latin(d, false));
    }
    rewriteTokens(l, texts);
  }

  // ---- one unit -----------------------------------------------------------------------------------------------------
  // Realises a clause and strips the final mark the realiser adds.
  void realiseClause(const realise::LaClause& cl0, const Options& opt, cue::Latin& out, std::vector<Reason>& reasons,
                     std::vector<std::string>& flags) {
    realise::LaClause cl = cl0;
    cl.punct = ".";
    realise::RealiseOptions ro;
    ro.macrons = true;
    ro.emoji = opt.emoji;
    ro.emojiInText = false;
    ro.fidelity = opt.fidelity;
    ro.glossary = glossary_;
    realise::LaSentence ls;
    realiser_->realise(cl, ro, ls);
    if (!ls.text.empty() && ls.text.back() == '.') ls.text.pop_back();
    out.text = ls.text;
    out.tokens = ls.tokens;
    for (const Reason& r : ls.reasons) reasons.push_back(r);
    for (const std::string& f : ls.flags) addFlag(flags, f);
  }

  void tokenInfo(const std::string& word, rules::TokenView& t, bool preferAdverb = false) const {
    morph::Token mt;
    morph::analyseLatin(*la_, word, mt);
    t.text = word;
    t.display = word;
    if (!mt.analyses.empty()) {
      // the reading of the best tier ("es" = sum, not the letter S; "mē" = ego), first one on ties
      size_t best = 0;
      uint8_t bestTier = 9;
      for (size_t i = 0; i < mt.analyses.size(); ++i) {
        const lex::Lemma l = la_->lemma(mt.analyses[i].lemma);
        const uint8_t tr = cd_->effectiveTier(l.key, l.pos, l.tier);
        if (tr < bestTier) { bestTier = tr; best = i; }
      }
      // C22: a one-word adverbial phrase of the phrasebook ("at first" -> prīmō) is the adverb, not prīmus
      if (preferAdverb)
        for (size_t i = 0; i < mt.analyses.size(); ++i) {
          const uint8_t lp = la_->lemma(mt.analyses[i].lemma).pos;
          if (lp == feat::Adv || lp == feat::Particle) { best = i; break; }
        }
      const lex::Analysis& a = mt.analyses[best];
      t.lemmaId = a.lemma;
      t.hasLemma = true;
      t.features = realise::featureView(morph::packedOf(*la_, a));
      const lex::Lemma l = la_->lemma(a.lemma);
      t.tier = cd_->effectiveTier(l.key, l.pos, l.tier);
    }
    if (t.hasLemma)   // phrasebook words get the reader's vowel quantities too (macron_overrides.tsv)
      if (const curated::MacronOverride* ov = cd_->macronOverride(la_->lemma(t.lemmaId).key))
        t.text = t.display = realise::Macrons::apply(word, true, ov);
  }

  // Phrasebook piece: the Latin template with its slots realised.
  void renderPhrase(const frame::PhraseMatch& m, const SemSentence& s, const transfer::Settings& st,
                    transfer::Memory& mem, const Options& opt, cue::Latin& out, std::vector<Reason>& reasons,
                    std::vector<std::string>& flags, std::vector<transfer::Choice>& choices, std::vector<int>& covered,
                    std::vector<std::string>& unknown) {
    std::string latin = m.latin;
    // plural variant from the note ("plural: salvēte") when the addressee is a group
    if (mem.addresseePlural && latin.find(' ') == std::string::npos) {
      const size_t p = m.note.find("plural");
      if (p != std::string::npos) {
        size_t a = p + 6;
        while (a < m.note.size() && (m.note[a] == ':' || m.note[a] == ' ')) ++a;
        size_t b = a;
        while (b < m.note.size() && m.note[b] != ' ' && m.note[b] != ',' && m.note[b] != ';') ++b;
        if (b > a) latin = m.note.substr(a, b - a);
      }
    }
    // C19: an alternative of several words on both sides ("mī amīce/mea amīca") is chosen whole by the speaker
    // glossary; a one-word right side ("ō mē miserum/miseram") alternates the word before the slash only
    {
      const size_t sl = latin.find('/');
      if (sl != std::string::npos && latin.find('/', sl + 1) == std::string::npos && latin.find('{') == std::string::npos) {
        const std::string right = latin.substr(sl + 1), left = latin.substr(0, sl);
        auto words = [](const std::string& x) { return std::count(x.begin(), x.end(), ' ') + 1; };
        // C24: the two sides are parallel word by word (same first letters: "mī amīce/mea amīca"); "valdē
        // perturbātus/perturbāta sum" alternates one word only (whole sides lost "sum" or "valdē")
        auto parallel = [&]() {
          std::vector<std::string> a, b;
          for (const std::string* side : {&left, &right}) {
            std::vector<std::string>& v = side == &left ? a : b;
            size_t i = 0;
            while (i < side->size()) {
              size_t j = side->find(' ', i);
              if (j == std::string::npos) j = side->size();
              if (j > i) v.push_back(text::latin_key(side->substr(i, j - i)));
              i = j + 1;
            }
          }
          if (a.size() != b.size()) return false;
          for (size_t i = 0; i < a.size(); ++i)
            if (a[i].empty() || b[i].empty() || a[i][0] != b[i][0]) return false;
          return true;
        };
        if (right.find(' ') != std::string::npos && words(left) == words(right) && parallel()) {
          char gg = st.speakerGender;
          if (st.flipSpeakerGender) gg = gg == 'f' ? 'm' : 'f';
          latin = gg == 'f' ? right : latin.substr(0, sl);
          addFlag(flags, "speaker-gender");
        }
      }
    }
    std::vector<std::string> ws;
    {
      size_t a = 0;
      while (a < latin.size()) {
        while (a < latin.size() && latin[a] == ' ') ++a;
        size_t b = a;
        while (b < latin.size() && latin[b] != ' ') ++b;
        if (b > a) ws.push_back(latin.substr(a, b - a));
        a = b;
      }
    }
    char g = st.speakerGender;
    if (st.flipSpeakerGender) g = g == 'f' ? 'm' : 'f';
    // C19: a nominative slot that is plural ("I like Roman roads" -> {1:nom} mihi placent): the verb of the row takes
    // the plural form its note gives ("plural: placent")
    std::string pluralWord;
    {
      const size_t p = m.note.find("plural:");
      if (p != std::string::npos) {
        size_t a = p + 7;
        while (a < m.note.size() && m.note[a] == ' ') ++a;
        size_t b = a;
        while (b < m.note.size() && m.note[b] != ' ' && m.note[b] != ',' && m.note[b] != ';') ++b;
        pluralWord = m.note.substr(a, b - a);
      }
    }
    bool nomPlural = false;
    for (const std::string& w0 : ws) {
      if (w0.size() < 3 || w0[0] != '{') continue;
      const size_t close = w0.find('}');
      const std::string inner = w0.substr(1, close == std::string::npos ? std::string::npos : close - 1);
      const size_t colon = inner.find(':');
      const int k = std::atoi(inner.substr(0, colon).c_str()) - 1;
      const std::string cs = colon == std::string::npos ? std::string() : inner.substr(colon + 1);
      if (k < 0 || (size_t)k >= m.slots.size() || (!cs.empty() && cs != "nom")) continue;
      const frame::PhraseSlot& sl = m.slots[(size_t)k];
      if (sl.kind == frame::SlotKind::NP && (sl.np.number == 2 || !sl.np.coord.empty())) nomPlural = true;
    }
    for (const std::string& w0 : ws) {
      std::string w = w0;
      if (w.size() > 2 && w[0] == '{') {
        const size_t close = w.find('}');
        const std::string inner = w.substr(1, close == std::string::npos ? std::string::npos : close - 1);
        const std::string tail = close == std::string::npos ? std::string() : w.substr(close + 1);
        const size_t colon = inner.find(':');
        const int k = std::atoi(inner.substr(0, colon).c_str()) - 1;
        const std::string cs = colon == std::string::npos ? std::string() : inner.substr(colon + 1);
        if (k < 0 || (size_t)k >= m.slots.size()) continue;
        const frame::PhraseSlot& sl = m.slots[(size_t)k];
        transfer::ClauseOut co;
        cue::Latin piece;
        std::vector<Reason> pr;
        if (sl.kind == frame::SlotKind::Wh) {
          // an indirect question ("that depends on {WH}" -> id pendet ex eō quō īre vīs); {1:subj} asks for the
          // subjunctive ("nihil interest quā viā eās")
          if (sl.wh.empty()) continue;
          xfer_->clause(sl.wh[0], s, st, mem, co);
          if (cs == "subj") co.clause.pred.mood = feat::Subjunctive;
          co.clause.punct = ".";
          realiseClause(co.clause, opt, piece, pr, flags);
          for (const std::string& f : co.flags) addFlag(flags, f);
          if (!piece.tokens.empty()) {
            std::vector<std::string> tx;
            for (const auto& t : piece.tokens) tx.push_back(t.text);
            decapitalise(tx[0]);
            rewriteTokens(piece, tx);
            decapitalise(piece.tokens[0].display);
          }
        } else if (sl.kind == frame::SlotKind::VP) {
          if (sl.vp.empty()) continue;
          xfer_->clause(sl.vp[0], s, st, mem, co);
          realise::LaClause main;
          main.type = realise::ClauseType::Frag;
          realise::LaSub sub;
          sub.rel = realise::SubRel::AccInf;
          co.clause.hasSubject = false;
          co.clause.type = realise::ClauseType::Decl;
          sub.clause.push_back(co.clause);
          main.subs.push_back(sub);
          realiseClause(main, opt, piece, pr, flags);
          if (!piece.tokens.empty() && !(piece.tokens[0].hasLemma &&
                                         (la_->lemma(piece.tokens[0].lemmaId).flags & lex::ProperName))) {
            bool name = false;   // C17: a slot inside a phrase takes no sentence capital ("nōn licet tibi calceum ...")
            for (const Reason& r : pr)
              if (r.tokenIndex == 0 && r.kind == "name") name = true;
            if (!name) {
              std::vector<std::string> tx;
              for (const auto& t : piece.tokens) tx.push_back(t.text);
              decapitalise(tx[0]);
              rewriteTokens(piece, tx);
              decapitalise(piece.tokens[0].display);
            }
          }
        } else if (sl.kind == frame::SlotKind::Adj) {
          transfer::Choice ch;
          ch.token = sl.adj.token;
          const uint32_t id = xfer_->select(sl.adj.lemma, feat::Adj, {}, false, false, st, ch);
          co.choices.push_back(ch);
          co.covered.push_back(sl.adj.token);
          if (id != lex::kNoLemma && !cs.empty()) {
            // C24: an adjective slot with a case and gender ("{1:abl.m}"): it agrees with a noun written in the row
            // ("diē {1:abl.m} post merīdiem" -> diē aprīcō post merīdiem)
            const size_t dot = cs.find('.');
            realise::LaNP x;
            x.head = id;
            x.case_ = curated::parseCase(cs.substr(0, dot));
            if (!x.case_) x.case_ = feat::Nom;
            const std::string gs = dot == std::string::npos ? std::string() : cs.substr(dot + 1);
            x.gender = gs == "f" ? (uint8_t)feat::F : gs == "n" ? (uint8_t)feat::N : (uint8_t)feat::M;
            realise::LaClause fc;
            fc.type = realise::ClauseType::Frag;
            fc.hasObject = true;
            fc.object = x;
            realiseClause(fc, opt, piece, pr, flags);
            if (!piece.tokens.empty()) {
              std::vector<std::string> tx;
              for (const auto& t : piece.tokens) tx.push_back(t.text);
              decapitalise(tx[0]);
              rewriteTokens(piece, tx);
              decapitalise(piece.tokens[0].display);
            }
          } else if (id != lex::kNoLemma) {
            realise::LaClause fc;
            fc.type = realise::ClauseType::Frag;
            realise::LaAdj a;
            a.lemma = id;
            fc.predAdj.push_back(a);
            // C22: the adjective agrees with the noun of the phrase ("What a big house!" -> Quam magna domus!)
            for (const frame::PhraseSlot& ns : m.slots) {
              if (ns.kind != frame::SlotKind::NP) continue;
              transfer::ClauseOut scratch;
              transfer::Memory mcopy = mem;
              const realise::LaNP nx = xfer_->np(ns.np, s, st, mcopy, scratch);
              uint8_t gg2 = nx.gender;
              if (!gg2 && nx.head != lex::kNoLemma) {
                const uint8_t lg = la_->lemma(nx.head).gender;
                gg2 = (lg == feat::F || lg == feat::FN) ? (uint8_t)feat::F : lg == feat::N ? (uint8_t)feat::N : lg ? (uint8_t)feat::M : (uint8_t)0;
              }
              if (gg2) fc.predGender = gg2;
              fc.predNumber = nx.number;
              break;
            }
            realiseClause(fc, opt, piece, pr, flags);
            if (!piece.tokens.empty()) {   // a slot inside a phrase: no sentence capital ("Quam mīrus hortus")
              std::vector<std::string> tx;
              for (const auto& t : piece.tokens) tx.push_back(t.text);
              decapitalise(tx[0]);
              rewriteTokens(piece, tx);
              decapitalise(piece.tokens[0].display);
            }
          } else {
            co.unknownWords.push_back(sl.adj.lemma);
          }
        } else {
          realise::LaNP x = xfer_->np(sl.np, s, st, mem, co);
          x.case_ = cs.empty() ? (uint8_t)feat::Nom : curated::parseCase(cs);
          if (!x.case_) x.case_ = feat::Nom;
          realise::LaClause fc;
          fc.type = realise::ClauseType::Frag;
          fc.hasObject = true;
          fc.object = x;
          realiseClause(fc, opt, piece, pr, flags);
          if (!piece.tokens.empty() && !x.isName && !x.capitalise) {
            bool name = false;
            for (const Reason& r : pr)
              if (r.tokenIndex == 0 && r.kind == "name") name = true;
            if (!name) {
              std::string t0 = piece.tokens[0].text;
              decapitalise(t0);
              std::vector<std::string> tx;
              for (const auto& t : piece.tokens) tx.push_back(t.text);
              tx[0] = t0;
              rewriteTokens(piece, tx);
              decapitalise(piece.tokens[0].display);
            }
          }
        }
        choices.insert(choices.end(), co.choices.begin(), co.choices.end());
        covered.insert(covered.end(), co.covered.begin(), co.covered.end());
        unknown.insert(unknown.end(), co.unknownWords.begin(), co.unknownWords.end());
        const int base = (int)out.tokens.size();
        for (Reason r : pr) { if (r.tokenIndex >= 0) r.tokenIndex += base; reasons.push_back(r); }
        piece.text += tail;
        cue::append(out, piece);
        continue;
      }
      // literal Latin word: "(tibi)" optional, "miserum/miseram" by speaker gender
      std::string punct;
      while (!w.empty() && (w.back() == ',' || w.back() == '!' || w.back() == '?' || w.back() == ';')) {
        punct.insert(punct.begin(), w.back());
        w.pop_back();
      }
      if (w.size() > 2 && w.front() == '(' && w.back() == ')') w = w.substr(1, w.size() - 2);
      if (nomPlural && !pluralWord.empty() && w.size() >= 4 && pluralWord.size() >= 4 &&
          text::latin_key(w).substr(0, 4) == text::latin_key(pluralWord).substr(0, 4))
        w = pluralWord;
      const size_t slash = w.find('/');
      if (slash != std::string::npos) {
        if (g == 'f') w = w.substr(slash + 1);
        else w = w.substr(0, slash);
        addFlag(flags, "speaker-gender");
      }
      cue::Latin one;
      rules::TokenView t;
      tokenInfo(w, t, ws.size() == 1 && (m.reg == "adv" || m.reg == "tail" || m.reg == "answer" || m.reg == "conn" ||
                                         m.reg == "lead"));
      one.text = t.text + punct;
      t.start = 0;
      t.end = (int)t.text.size();
      one.tokens.push_back(t);
      cue::append(out, one);
    }
    for (int k = m.first; k <= m.last; ++k) covered.push_back(k);
    // C19: a slot's content words count as translated only when the slot's translation used them (a word lost inside
    // a slot shows in A7, e.g. a dropped possessive); function words of the slot are covered by the row
    for (const frame::PhraseSlot& sl : m.slots)
      for (int k = sl.first; k <= sl.last; ++k) {
        if (k < 0 || (size_t)k >= s.tokens.size()) continue;
        const std::string& u = s.tokens[(size_t)k].upos;
        const bool content = u == "NOUN" || u == "PROPN" || u == "VERB" || u == "ADJ" || u == "ADV" || u == "NUM" || u == "PRON";
        if (!content || sl.kind == frame::SlotKind::Wh || sl.kind == frame::SlotKind::VP || sl.kind == frame::SlotKind::Name)
          covered.push_back(k);
      }
    // C23: a Medieval / ecclesiastical row says so in the reason data ("register":"eccl")
    bool eccl = false;
    {
      const std::vector<curated::PhraseEntry>& book = st.lang == frame::SrcLang::Es ? cd_->phrasebookEs() : cd_->phrasebook();
      eccl = m.entry >= 0 && (size_t)m.entry < book.size() && book[(size_t)m.entry].eccl;
    }
    reasons.push_back(Reason{out.tokens.empty() ? -1 : 0, "phrasebook", m.pattern + " -> " + m.latin,
                             "{\"pattern\":\"" + jsonEscape(m.pattern) + "\",\"latin\":\"" + jsonEscape(m.latin) +
                                 "\",\"tier\":" + std::to_string((int)m.tier) + (eccl ? ",\"register\":\"eccl\"" : "") + "}"});
  }

  // C15: word-by-word rendering for a sentence the analysis could not parse: each content word in its Latin
  // dictionary form (the best candidate), names as written, "not" as nōn, unknown words in brackets.
  void literalRender(const SemSentence& s, const transfer::Settings& st, SentOut& so, std::vector<int>& covered) {
    cue::Latin L;
    so.choices.clear();
    so.unknown.clear();
    for (size_t i = 0; i < s.tokens.size(); ++i) {
      const nlp::Token& t = s.tokens[i];
      std::string word;
      uint32_t lemma = lex::kNoLemma;
      uint8_t pos = t.upos == "NOUN" ? feat::Noun : t.upos == "VERB" ? feat::Verb : t.upos == "ADJ" ? feat::Adj
                  : t.upos == "ADV" ? feat::Adv : 0;
      if (t.lower == "not" || t.lower == "n't" || t.lower == "never") {
        word = t.lower == "never" ? "numquam" : "nōn";
        lemma = xfer_->latin(word.c_str(), feat::Adv);
      } else if (t.upos == "PROPN" && !(i == 0 && la_)) {
        word = t.text;
      } else if (pos || t.upos == "PROPN") {
        if (!pos) pos = feat::Adj;   // a capitalised first word: try it as a word
        if (t.upos == "PROPN" && xfer_->latin(t.lower.c_str()) == lex::kNoLemma) {
          transfer::Choice probe;
          if (xfer_->select(t.lower, feat::Adj, {}, false, false, st, probe) == lex::kNoLemma &&
              xfer_->select(t.lower, feat::Noun, {}, false, false, st, probe) != lex::kNoLemma)
            pos = feat::Noun;
        }
        transfer::Choice ch;
        ch.token = (int)i;
        lemma = xfer_->select(t.upos == "PROPN" ? t.lower : t.lemma.empty() ? t.lower : t.lemma, pos, {}, false, false, st, ch);
        so.choices.push_back(ch);
        if (lemma != lex::kNoLemma) word = morph::displayForm(la_->lemma(lemma).head, true);
        else { word = "[" + t.text + "]"; so.unknown.push_back(t.text); }
      } else {
        continue;
      }
      covered.push_back((int)i);
      cue::Latin one;
      rules::TokenView tv;
      tokenInfo(word, tv);
      tv.text = tv.display = word;
      if (lemma != lex::kNoLemma) { tv.lemmaId = lemma; tv.hasLemma = true; }
      tv.unknown = word[0] == '[';
      tv.start = 0;
      tv.end = (int)word.size();
      one.text = word;
      one.tokens.push_back(tv);
      cue::append(L, one);
    }
    so.latin = L;
    so.srcOffset.assign(L.tokens.size(), -1);
    so.reasons.erase(std::remove_if(so.reasons.begin(), so.reasons.end(), [](const Reason& r) { return r.tokenIndex >= 0; }),
                     so.reasons.end());
    so.reasons.push_back(Reason{-1, "form", "could not parse: the words are given one by one (dictionary forms)", ""});
  }

  // ---- one sentence ---------------------------------------------------------------------------------------------------
  // Parser-failure fallback (C2b): the sentence split at ", or" / ", and" / ";" and each piece translated on its own,
  // joined with ";"; Check.
  void speechSplit(const std::string& text, const std::vector<size_t>& pts, const frame::FrameBuilder& fb,
                   const Options& opt, const Context& ctx, transfer::Memory& mem, const transfer::Settings& st,
                   SentOut& so) {
    std::vector<size_t> cuts = pts;
    cuts.push_back(text.size());
    size_t a = 0;
    for (size_t cut : cuts) {
      std::string part = text.substr(a, cut - a);
      while (!part.empty() && (part.back() == ' ' || part.back() == ',' || part.back() == ';')) part.pop_back();
      const size_t off = a;
      a = cut;
      if (part.empty()) continue;
      SentOut po;
      speech(part, fb, opt, ctx, mem, st, po, false, false);
      if (po.latin.text.empty()) continue;
      if (!so.latin.text.empty()) {
        while (!so.latin.text.empty() && endsWithAny(so.latin.text, ".,;:")) so.latin.text.pop_back();
        so.latin.text += ";";
        if (!po.latin.tokens.empty() && !(po.latin.tokens[0].hasLemma &&
                                          (la_->lemma(po.latin.tokens[0].lemmaId).flags & lex::ProperName))) {
          std::vector<std::string> tx;
          for (const auto& t : po.latin.tokens) tx.push_back(t.text);
          decapitalise(tx[0]);
          rewriteTokens(po.latin, tx);
          decapitalise(po.latin.tokens[0].display);
        }
      }
      const int base = (int)so.latin.tokens.size();
      cue::append(so.latin, po.latin);
      for (int o : po.srcOffset) so.srcOffset.push_back(o >= 0 ? o + (int)off : -1);
      for (Reason r : po.reasons) { if (r.tokenIndex >= 0) r.tokenIndex += base; so.reasons.push_back(r); }
      for (const std::string& f : po.flags) addFlag(so.flags, f);
      so.choices.insert(so.choices.end(), po.choices.begin(), po.choices.end());
      so.unknown.insert(so.unknown.end(), po.unknown.begin(), po.unknown.end());
      so.missing.insert(so.missing.end(), po.missing.begin(), po.missing.end());
      so.minMargin = std::min(so.minMargin, po.minMargin);
      if (po.vocGender) so.vocGender = po.vocGender;   // C24
      so.invites = po.invites;
    }
    addFlag(so.flags, "frame-fallback");
    so.reasons.push_back(Reason{-1, "form", "the sentence was analysed in pieces (parser fallback): check the structure", ""});
  }

  // C23 (D18): phrasebook rows of the same pattern, one marked eccl and the other not, are twins: the ecclesiastical
  // and the classical rendering ("I'm sorry" -> Habeās mē excūsātum / Ignōsce mihi). The twin of a row, or -1; with
  // latinity "classical" an eccl twin is never offered.
  int phraseTwin(int entry, frame::SrcLang lang, bool classical) const {
    const std::vector<curated::PhraseEntry>& book = lang == frame::SrcLang::Es ? cd_->phrasebookEs() : cd_->phrasebook();
    if (entry < 0 || (size_t)entry >= book.size()) return -1;
    const curated::PhraseEntry& e = book[(size_t)entry];
    const std::string pat = text::lower(e.pattern);
    for (size_t q = 0; q < book.size(); ++q) {
      if ((int)q == entry || book[q].eccl == e.eccl || (classical && book[q].eccl)) continue;
      if (text::lower(book[q].pattern) == pat) return (int)q;
    }
    return -1;
  }
  // C23: the rows of transfer::Settings::phraseSwaps replace the matched rows (alternative renderings)
  void swapPhrases(SemSentence& s, const transfer::Settings& st) const {
    if (st.phraseSwaps.empty()) return;
    const std::vector<curated::PhraseEntry>& book = st.lang == frame::SrcLang::Es ? cd_->phrasebookEs() : cd_->phrasebook();
    for (frame::Unit& u : s.units) {
      if (u.type != frame::Unit::Phrase) continue;
      for (const auto& sw : st.phraseSwaps)
        if (u.phrase.entry == sw.first && sw.second >= 0 && (size_t)sw.second < book.size()) {
          const curated::PhraseEntry& e = book[(size_t)sw.second];
          u.phrase.entry = sw.second;
          u.phrase.latin = e.latin;
          u.phrase.reg = e.reg;
          u.phrase.note = e.note;
          u.phrase.tier = e.tier;
          break;
        }
    }
  }

  // C24: the gender of a person addressed: a name of the project glossary or names_la.tsv, a title before it or as
  // the address itself (Mr., Sir -> M; Mrs., Miss, Madam -> F), else the gender of the Latin noun chosen for it
  // (puer, avus -> M; puella, avia -> F); neuter or unknown -> 0
  uint8_t addressGender(const frame::SemNP& np, const SemSentence& s, const transfer::Settings& st,
                        const transfer::Memory& mem, const Context& ctx) const {
    if (np.isPronoun) return 0;
    int first = np.token;
    for (int t : np.tokens) if (t >= 0 && (first < 0 || t < first)) first = t;
    // a title before the head, inside the NP ("Mr./DET") or just before it
    for (int k = np.token - 1; k >= 0 && k >= first - 2; --k) {
      if ((size_t)k >= s.tokens.size()) break;
      std::string w = text::lower(s.tokens[(size_t)k].text);
      if (!w.empty() && w.back() == '.') w.pop_back();
      if (w == "mr" || w == "mister" || w == "sir") return feat::M;
      if (w == "mrs" || w == "miss" || w == "ms" || w == "madam" || w == "missus" || w == "lady") return feat::F;
      if (k < first && w != "." && w != ",") break;
    }
    std::string head = np.surface.empty() ? np.head : np.surface;
    for (const rules::GlossaryEntry& g : ctx.glossary)
      if (!g.gender.empty() && text::lower(g.name) == text::lower(head))
        return g.gender[0] == 'f' || g.gender[0] == 'F' ? (uint8_t)feat::F : g.gender[0] == 'm' || g.gender[0] == 'M' ? (uint8_t)feat::M : (uint8_t)0;
    if (!head.empty()) {
      std::string cap = head;
      if (cap[0] >= 'a' && cap[0] <= 'z') cap[0] = (char)(cap[0] - 'a' + 'A');
      if (const curated::NameEntry* ne = cd_->nameByEnglish(cap))
        return ne->gender == feat::F ? (uint8_t)feat::F : ne->gender == feat::M ? (uint8_t)feat::M : (uint8_t)0;
    }
    if (np.isName) return 0;
    // a noun of address that says the sex ("Grandfather", "my girl"); "child", "friend", an animal: unknown
    const std::string h = text::lower(np.head);
    static const char* const kM[] = {"grandfather", "grandpa", "father", "dad", "daddy", "papa", "uncle", "brother",
                                     "son", "master", "boy", "sir", "king", "prince", "lord", "gentleman", "man",
                                     "lad", "mister", "grandson", "nephew", "husband"};
    static const char* const kF[] = {"grandmother", "grandma", "mother", "mom", "mum", "mommy", "mummy", "mama", "aunt",
                                     "auntie", "sister", "daughter", "mistress", "girl", "madam", "queen", "princess",
                                     "lady", "woman", "granddaughter", "niece", "wife"};
    for (const char* w : kM)
      if (h == w) return feat::M;
    for (const char* w : kF)
      if (h == w) return feat::F;
    (void)st;
    (void)mem;
    return 0;
  }

  // C24: SentOut::vocGender / invites from the analysed sentence
  void addressInfo(const SemSentence& s, const transfer::Settings& st, const transfer::Memory& mem, const Context& ctx,
                   SentOut& so) const {
    bool imp = false, onlyAddress = !s.units.empty(), anyVoc = false;
    for (const frame::Unit& u : s.units) {
      if (u.type == frame::Unit::Phrase) {
        const std::string pl = text::lower(u.phrase.pattern);
        const bool leaving = pl.find("bye") != std::string::npos || pl.find("farewell") != std::string::npos ||
                             pl.find("good night") != std::string::npos;
        if (u.phrase.reg == "imp") imp = true;
        if ((u.phrase.reg != "polite" && u.phrase.reg != "greet" && u.phrase.reg != "excl") || leaving) onlyAddress = false;
        continue;
      }
      const frame::SemFrame& f = u.frame;
      if (u.vocative) {
        anyVoc = true;
        if (const uint8_t g = addressGender(f.subject, s, st, mem, ctx)) so.vocGender = g;
        continue;
      }
      if (f.type == frame::Kind::Imp) imp = true;
      // a name alone ("Alice.", "Mr. Fox.") is a fragment holding the name
      if (f.type == frame::Kind::Frag && f.hasSubject && !f.subject.isPronoun && !f.hasPred && f.predAdj.empty() &&
          f.obliques.empty()) {
        const uint8_t g = addressGender(f.subject, s, st, mem, ctx);
        if (f.subject.isName || g) {
          anyVoc = true;
          if (g) so.vocGender = g;
          continue;
        }
      }
      for (const frame::SemNP& v : f.vocatives) {
        anyVoc = true;
        if (const uint8_t g = addressGender(v, s, st, mem, ctx)) so.vocGender = g;
      }
      onlyAddress = false;
    }
    so.invites = s.question || imp || (onlyAddress && anyVoc);
  }

  // C24: spans of quoted sound words: one or two words between straight or curly double quotes, each an English
  // interjection (or a word english.vpl does not know) that is not a translatable answer word (yes, no, hello ...);
  // the span (quotes included) is replaced by "it" and spaces
  void quotedSounds(std::string& t, std::vector<std::pair<size_t, size_t>>& spans) const {
    auto openAt = [&](size_t i, size_t& len) {
      if (t[i] == '"') { len = 1; return true; }
      if (i + 2 < t.size() && (unsigned char)t[i] == 0xE2 && (unsigned char)t[i + 1] == 0x80 &&
          ((unsigned char)t[i + 2] == 0x9C || (unsigned char)t[i + 2] == 0x9D)) { len = 3; return true; }
      return false;
    };
    for (size_t i = 0; i < t.size(); ++i) {
      size_t ol = 0;
      if (!openAt(i, ol)) continue;
      size_t j = i + ol;
      size_t cl = 0;
      size_t e = j;
      while (e < t.size() && !openAt(e, cl)) ++e;
      if (e >= t.size()) break;
      std::string inner = t.substr(j, e - j);
      while (!inner.empty() && (inner.back() == ',' || inner.back() == '!' || inner.back() == '.' || inner.back() == '?')) inner.pop_back();
      std::vector<std::string> ws;
      bool okWords = !inner.empty() && inner.size() <= 24;
      {
        size_t a = 0;
        while (a < inner.size() && okWords) {
          size_t b = inner.find(' ', a);
          if (b == std::string::npos) b = inner.size();
          const std::string w = text::lower(inner.substr(a, b - a));
          if (!w.empty()) ws.push_back(w);
          for (char ch : w) okWords = okWords && (std::isalpha((unsigned char)ch) || ch == '-');
          a = b + 1;
        }
      }
      okWords = okWords && !ws.empty() && ws.size() <= 2;
      for (const std::string& w : ws) {
        if (!okWords) break;
        static const char* const kWords[] = {"yes", "no", "hello", "hi", "goodbye", "bye", "oh", "ah", "please", "thanks",
                                             "ok", "okay", "sorry", "help", "hooray", "alas", "well", "why", "what"};
        for (const char* x : kWords) okWords = okWords && w != x;
        std::vector<lex::Analysis> an;
        en_->lookup(text::en_key(w), an);
        bool intj = an.empty();
        for (const lex::Analysis& a : an) intj = intj || en_->lemma(a.lemma).pos == feat::Intj;
        okWords = okWords && intj;
      }
      if (okWords && e - i >= 2) {
        const size_t end = e + cl;
        spans.emplace_back(i, end);
        for (size_t k = i; k < end; ++k) t[k] = ' ';
        t[i] = 'i';
        t[i + 1] = 't';
      }
      i = e + cl - 1;
    }
  }

  // C24: the noun that ends the previous song line (no punctuation after it) when this line opens with a relative
  // word (that, which, who, where) followed by more words; empty otherwise
  std::string songAntecedent(const std::string& prev, const std::string& line) const {
    if (prev.empty() || line.empty()) return std::string();
    size_t e = prev.size();
    while (e > 0 && prev[e - 1] == ' ') --e;
    if (e == 0 || !std::isalpha((unsigned char)prev[e - 1])) return std::string();
    size_t b = e;
    while (b > 0 && std::isalpha((unsigned char)prev[b - 1])) --b;
    const std::string last = text::lower(prev.substr(b, e - b));
    size_t a = 0;
    while (a < line.size() && line[a] == ' ') ++a;
    size_t w = a;
    while (w < line.size() && std::isalpha((unsigned char)line[w])) ++w;
    const std::string first = text::lower(line.substr(a, w - a));
    if (first != "that" && first != "which" && first != "who") return std::string();
    if (line.find(' ', w) == std::string::npos) return std::string();
    // the words of the previous line, lower case
    std::vector<std::string> ws;
    {
      size_t i = 0;
      while (i < e) {
        while (i < e && !std::isalpha((unsigned char)prev[i])) ++i;
        size_t j = i;
        while (j < e && (std::isalpha((unsigned char)prev[j]) || prev[j] == '\'')) ++j;
        if (j > i) ws.push_back(text::lower(prev.substr(i, j - i)));
        i = j;
      }
    }
    auto has = [&](const std::string& word, uint8_t pos) {
      std::vector<lex::Analysis> an;
      en_->lookup(text::en_key(word), an);
      for (const lex::Analysis& x : an)
        if (en_->lemma(x.lemma).pos == pos) return true;
      return false;
    };
    auto plainNoun = [&](const std::string& word) {
      return has(word, feat::Noun) && !has(word, feat::Det) && !has(word, feat::Prep) && !has(word, feat::Pron);
    };
    if (ws.empty() || !plainNoun(last)) return std::string();
    // a noun inside a prepositional phrase that closes the line ("the land beyond the hills"): the noun before the
    // preposition is the antecedent
    size_t k = ws.size() - 1;
    size_t p = k;
    while (p > 0 && (ws[p - 1] == "the" || ws[p - 1] == "a" || ws[p - 1] == "an" || has(ws[p - 1], feat::Adj))) --p;
    if (p > 0 && has(ws[p - 1], feat::Prep) && !has(ws[p - 1], feat::Verb)) {
      size_t q = p - 1;
      while (q > 0 && !plainNoun(ws[q - 1])) {
        if (has(ws[q - 1], feat::Verb) && !has(ws[q - 1], feat::Noun)) { q = 0; break; }
        --q;
      }
      if (q > 0) return "the " + ws[q - 1];
    }
    (void)k;
    return "the " + last;
  }

  // C24: clause boundaries of a sentence the parser could not build: after a comma, and before and / but / or /
  // because / when / while / so / then (never at the very start or end)
  static std::vector<size_t> clausePoints(const std::string& t) {
    std::vector<size_t> pts;
    for (size_t i = 1; i + 2 < t.size(); ++i) {
      if (t[i] == ',' && t[i + 1] == ' ') { pts.push_back(i + 2); continue; }
      if (t[i] != ' ' || t[i - 1] == ',') continue;
      size_t j = i + 1;
      while (j < t.size() && std::isalpha((unsigned char)t[j])) ++j;
      const std::string w = text::lower(t.substr(i + 1, j - i - 1));
      if ((w == "and" || w == "but" || w == "or" || w == "because" || w == "when" || w == "while" || w == "so" ||
           w == "then") && j < t.size() && t[j] == ' ')
        pts.push_back(i + 1);
    }
    std::vector<size_t> out;
    for (size_t p : pts)
      if (p > 2 && p + 3 < t.size() && (out.empty() || p > out.back() + 2)) out.push_back(p);
    return out;
  }

  static void stutter(std::string& t) {
    auto wordAt = [&](size_t a) {
      size_t b = a;
      while (b < t.size() && (std::isalpha((unsigned char)t[b]) || t[b] == '\'')) ++b;
      return b;
    };
    for (size_t i = 0; i < t.size(); ++i) {
      if (!std::isalpha((unsigned char)t[i]) || (i > 0 && (std::isalpha((unsigned char)t[i - 1]) || t[i - 1] == '\''))) continue;
      const size_t e = wordAt(i);
      size_t p = e;
      size_t dots = 0;
      while (p < t.size() && (t[p] == '.' || t[p] == ' ' || t[p] == ',')) { dots += t[p] == '.'; ++p; }
      if (p + 2 < t.size() && (unsigned char)t[p] == 0xE2 && (unsigned char)t[p + 1] == 0x80 && (unsigned char)t[p + 2] == 0xA6) {
        dots = 3;
        p += 3;
        while (p < t.size() && t[p] == ' ') ++p;
      }
      if (dots < 2 || p >= t.size()) continue;
      // C24: a hesitation inside a verb group ("we must... be going", "you must... come"): the dots go
      {
        const std::string prev = text::lower(t.substr(i, e - i));
        static const char* const kOpen[] = {"should", "shouldn't", "would", "wouldn't", "could", "couldn't", "can",
                                            "can't", "must", "mustn't", "will", "won't", "shall", "might", "may",
                                            "not", "never", "to", "really", "just"};
        bool open = false;
        for (const char* w : kOpen) open = open || prev == w;
        if (open && p < t.size() && t[p] >= 'a' && t[p] <= 'z') {
          for (size_t k = e; k < p; ++k) if (t[k] == '.' || (unsigned char)t[k] >= 0x80) t[k] = ' ';
          i = p - 1;
          continue;
        }
      }
      const size_t e2 = wordAt(p);
      if (e2 - p != e - i || text::lower(t.substr(p, e2 - p)) != text::lower(t.substr(i, e - i))) continue;
      for (size_t k = e; k < e2; ++k) t[k] = ' ';
      i = e2;
    }
  }

  void speech(const std::string& text, const frame::FrameBuilder& fb, const Options& opt, const Context& ctx,
              transfer::Memory& mem, const transfer::Settings& st, SentOut& so, bool alternatives,
              bool allowSplit = true) {
    SemSentence s;
    const transfer::Memory memEntry = mem;   // C24: for the clause split before the word-by-word fallback
    // C17: editorial text in square brackets ("They are rusted [so badly] that ...") is analysed with the sentence
    // (the brackets read as spaces, so offsets stay) and its Latin words are put back in brackets at the end
    std::string parseText = text;
    std::vector<std::pair<size_t, size_t>> edSpans;
    if (st.lang == frame::SrcLang::En)
      for (size_t a0 = parseText.find('['); a0 != std::string::npos; a0 = parseText.find('[', a0 + 1)) {
        const size_t b0 = parseText.find(']', a0 + 1);
        if (b0 == std::string::npos) break;
        edSpans.emplace_back(a0, b0);
        parseText[a0] = ' ';
        parseText[b0] = ' ';
      }
    // C24: a hesitation that repeats a word after an ellipsis ("swim in... in the lake", "I... I know") is
    // read once: the ellipsis and the repeated word become spaces (offsets stay)
    if (st.lang == frame::SrcLang::En) stutter(parseText);
    // C24: a sound word in quotes ("woof", "moo") is kept as written, in its quotes: the parser reads the pronoun
    // "it" in its place (spaces keep the offsets) and the Latin word of that pronoun is replaced by the quoted word
    std::vector<std::pair<size_t, size_t>> soundSpans;
    if (st.lang == frame::SrcLang::En && en_) quotedSounds(parseText, soundSpans);
    fb.analyse(parseText, s, st.classical);
    swapPhrases(s, st);
    if (allowSplit && frame::FrameBuilder::troubled(s)) {
      const std::vector<size_t> pts = frame::FrameBuilder::splitPoints(text);
      if (!pts.empty()) { speechSplit(text, pts, fb, opt, ctx, mem, st, so); return; }
      // no split point: drop discourse words ("well", "oh", "so", "just", "really") and analyse again
      std::string simple;
      bool changed = false;
      {
        size_t i = 0;
        while (i < text.size()) {
          size_t j = i;
          while (j < text.size() && text[j] != ' ') ++j;
          std::string w = text.substr(i, j - i), low = text::lower(w);
          while (!low.empty() && (low.back() == ',' )) low.pop_back();
          const bool drop = low == "well" || low == "oh" || low == "so" || low == "just" || low == "really" ||
                            low == "um" || low == "uh" || low == "now";
          if (drop && i == 0 && j < text.size()) { changed = true; }
          else if (drop && i > 0 && (low == "just" || low == "really" || low == "um" || low == "uh")) { changed = true; }
          else { if (!simple.empty()) simple += ' '; simple += w; }
          i = j + 1;
        }
      }
      if (changed && !simple.empty()) {
        SemSentence s2;
        fb.analyse(simple, s2, st.classical);
        swapPhrases(s2, st);
        if (!frame::FrameBuilder::troubled(s2)) {
          s = std::move(s2);
          s.repairs.emplace_back("simplified");
        }
      }
      if (frame::FrameBuilder::troubled(s)) s.repairs.emplace_back("no-verb");
    }
    if (st.lang == frame::SrcLang::En) {
      addressInfo(s, st, mem, ctx, so);
      // C24: a person addressed by a noun of known gender ("Mother, are you tired?") sets the gender of "you" when no
      // name did (C22 Memory::addresseeGender)
      if (!mem.addresseeGender && so.vocGender) mem.addresseeGender = so.vocGender;
    }
    mem.sawFirst = false;
    std::vector<int> covered;
    std::vector<std::string> flags;
    struct UnitText { cue::Latin latin; std::vector<Reason> reasons; std::string sep; bool nameFirst = false;
                      int srcStart = -1; size_t choiceFrom = 0, choiceTo = 0; bool connFront = false;
                      int first = 0, last = -1; bool clause = false; std::string reg; };
    std::vector<UnitText> units;
    for (size_t ui = 0; ui < s.units.size(); ++ui) {
      const frame::Unit& u = s.units[ui];
      UnitText ut;
      ut.sep = u.sepAfter;
      ut.first = u.first;
      ut.last = u.last;
      ut.clause = u.type == frame::Unit::Clause && !u.vocative;
      if (u.type == frame::Unit::Phrase) ut.reg = u.phrase.reg;
      ut.srcStart = u.first < (int)s.tokens.size() ? s.tokens[(size_t)u.first].start : -1;
      ut.choiceFrom = so.choices.size();
      if (u.type == frame::Unit::Phrase) {
        ut.connFront = u.phrase.reg == "conn" && ui > 0;   // C15: "..., however;" -> Tamen ...
        renderPhrase(u.phrase, s, st, mem, opt, ut.latin, ut.reasons, flags, so.choices, covered, so.unknown);
        // connectors carried by the phrase ("Then go away." -> Abī igitur.)
        for (const std::string& k : u.frame.connectors) {
          const char* la = nullptr;
          if (k == "then") la = mem.prevFirst ? "deinde" : "igitur";
          else if (k == "and") la = "et";
          else if (k == "but") la = "sed";
          else if (k == "so") la = "itaque";
          else if (k == "now") la = "nunc";
          if (!la) continue;
          rules::TokenView t;
          tokenInfo(la, t);
          t.text = t.display = la;
          const bool second = std::string(la) == "igitur";
          cue::Latin w;
          w.text = la;
          t.start = 0;
          t.end = (int)w.text.size();
          w.tokens.push_back(t);
          if (second && !ut.latin.tokens.empty()) {
            // after the first word
            cue::Latin merged;
            cue::Latin first;
            const rules::TokenView& t0 = ut.latin.tokens[0];
            const size_t cut = (size_t)t0.end;
            size_t rest = cut;
            while (rest < ut.latin.text.size() && ut.latin.text[rest] != ' ') ++rest;   // keep "word," together
            first.text = ut.latin.text.substr(0, rest);
            first.tokens.push_back(t0);
            cue::Latin tail;
            if (rest < ut.latin.text.size()) {
              const size_t b = rest + 1;
              tail.text = ut.latin.text.substr(b);
              for (size_t i = 1; i < ut.latin.tokens.size(); ++i) {
                rules::TokenView x = ut.latin.tokens[i];
                x.start -= (int)b;
                x.end -= (int)b;
                tail.tokens.push_back(x);
              }
            }
            cue::append(merged, first);
            cue::append(merged, w);
            cue::append(merged, tail);
            for (Reason& r : ut.reasons)
              if (r.tokenIndex >= 1) ++r.tokenIndex;
            ut.latin = merged;
          } else {
            cue::Latin merged;
            cue::append(merged, w);
            cue::append(merged, ut.latin);
            for (Reason& r : ut.reasons)
              if (r.tokenIndex >= 0) ++r.tokenIndex;
            ut.latin = merged;
          }
          transfer::Choice cc;
          cc.token = u.frame.tokens.empty() ? -1 : u.first;
          cc.source = k;
          cc.lemma = t.lemmaId;
          cc.kind = "table";
          so.choices.push_back(cc);
          covered.push_back(u.first);
        }
      } else {
        transfer::ClauseOut co;
        if (u.vocative) xfer_->vocative(u.frame.subject, s, st, mem, co);
        else xfer_->clause(u.frame, s, st, mem, co);
        realiseClause(co.clause, opt, ut.latin, ut.reasons, flags);
        so.choices.insert(so.choices.end(), co.choices.begin(), co.choices.end());
        covered.insert(covered.end(), co.covered.begin(), co.covered.end());
        for (const std::string& f : co.flags) addFlag(flags, f);
        // unknown verbs: "[verb]" -> "[source]"; other unknown words appended in brackets
        std::vector<std::string> unk = co.unknownWords;
        std::vector<std::string> tx;
        bool changed = false;
        for (rules::TokenView& t : ut.latin.tokens) {
          tx.push_back(t.text);
          if (t.text == "[verb]" || t.text == "[?]") {
            for (const transfer::Choice& ch : co.choices)
              if (ch.unknown) { tx.back() = "[" + ch.source + "]"; break; }
            t.unknown = true;
            changed = true;
          }
          if (t.text.size() > 2 && t.text.front() == '[') t.unknown = true;
        }
        if (changed) rewriteTokens(ut.latin, tx);
        for (const std::string& w : unk) {
          if (ut.latin.text.find("[" + w + "]") != std::string::npos) continue;
          bool shown = false;
          for (const auto& t : ut.latin.tokens)
            if (t.text == "[" + w + "]") shown = true;
          if (shown) continue;
          cue::Latin x;
          rules::TokenView t;
          t.text = t.display = "[" + w + "]";
          t.unknown = true;
          t.start = 0;
          t.end = (int)t.text.size();
          x.text = t.text;
          x.tokens.push_back(t);
          cue::append(ut.latin, x);
        }
        so.unknown.insert(so.unknown.end(), unk.begin(), unk.end());
      }
      ut.choiceTo = so.choices.size();
      if (!ut.latin.tokens.empty() && ut.latin.tokens[0].hasLemma &&
          (la_->lemma(ut.latin.tokens[0].lemmaId).flags & lex::ProperName))
        ut.nameFirst = true;
      for (const Reason& r : ut.reasons)
        if (r.tokenIndex == 0 && r.kind == "name") ut.nameFirst = true;
      if (u.type == frame::Unit::Phrase && !ut.latin.tokens.empty()) {
        const std::string& f = ut.latin.tokens[0].text;
        if (!f.empty() && ((f[0] >= 'A' && f[0] <= 'Z') || (unsigned char)f[0] >= 0xC3)) ut.nameFirst = true;
      }
      if (!ut.latin.text.empty()) units.push_back(std::move(ut));
    }
    // C17: an adverbial phrase that stands inside a clause in the source ("so that in reality I may become ...") goes
    // into that clause: after its opening conjunction (ut, et, sed ...), else first
    for (size_t i = 1; i < units.size(); ++i) {
      if (units[i].clause || (units[i].reg != "adv" && units[i].reg != "answer") || units[i].latin.tokens.empty()) continue;
      size_t j = i;
      for (size_t q = 0; q < i; ++q)
        if (units[q].clause && units[q].first < units[i].first && units[i].last < units[q].last) j = q;
      if (j == i || units[j].latin.tokens.empty()) continue;
      cue::Latin& host = units[j].latin;
      const rules::TokenView& h0 = host.tokens[0];
      const std::string k0 = text::latin_key(h0.text);
      const bool conj = k0 == "ut" || k0 == "et" || k0 == "sed" || k0 == "nam" || k0 == "itaque" || k0 == "aut" ||
                        k0 == "ne" || k0 == "si" || k0 == "quia" || k0 == "cum";
      const size_t cut = conj ? (size_t)h0.end : 0;
      cue::Latin head, tail, merged;
      if (conj) {
        head.text = host.text.substr(0, cut);
        head.tokens.push_back(h0);
      }
      const size_t b = conj ? std::min(host.text.size(), cut + 1) : 0;
      tail.text = host.text.substr(b);
      for (size_t t = conj ? 1 : 0; t < host.tokens.size(); ++t) {
        rules::TokenView x = host.tokens[t];
        x.start -= (int)b;
        x.end -= (int)b;
        tail.tokens.push_back(x);
      }
      if (!conj && !tail.tokens.empty() &&
          !(tail.tokens[0].hasLemma && (la_->lemma(tail.tokens[0].lemmaId).flags & lex::ProperName))) {
        bool nm = false;
        for (const Reason& r : units[j].reasons)
          if (r.tokenIndex == 0 && r.kind == "name") nm = true;
        if (!nm) {
          std::vector<std::string> tt;
          for (const auto& t : tail.tokens) tt.push_back(t.text);
          decapitalise(tt[0]);
          rewriteTokens(tail, tt);
          decapitalise(tail.tokens[0].display);
        }
      }
      cue::Latin ph = units[i].latin;
      std::vector<std::string> tx;
      for (const auto& t : ph.tokens) tx.push_back(t.text);
      if (!tx.empty() && !(ph.tokens[0].hasLemma && (la_->lemma(ph.tokens[0].lemmaId).flags & lex::ProperName))) {
        decapitalise(tx[0]);
        rewriteTokens(ph, tx);
        decapitalise(ph.tokens[0].display);
      }
      while (!ph.text.empty() && endsWithAny(ph.text, ",;:")) ph.text.pop_back();
      cue::append(merged, head);
      cue::append(merged, ph);
      cue::append(merged, tail);
      for (Reason& r : units[j].reasons)
        if (r.tokenIndex >= (int)(conj ? 1 : 0)) r.tokenIndex += (int)ph.tokens.size();
      const int at = conj ? 1 : 0;
      for (Reason r : units[i].reasons) { if (r.tokenIndex >= 0) r.tokenIndex += at; units[j].reasons.push_back(r); }
      host = merged;
      units.erase(units.begin() + (long)i);
      --i;
    }
    // C17: an adverbial phrase that closes its clause in the source ("We found the house at last.", "I cannot move
    // them at all;") stands before the clause's final verb group in Latin (tandem invēnimus, omnīnō movēre nōn possim)
    for (size_t i = 1; i < units.size(); ++i) {
      UnitText& ph = units[i];
      UnitText& host = units[i - 1];
      if (ph.clause || ph.reg != "adv" || !host.clause || !host.sep.empty() || ph.latin.tokens.empty() ||
          host.latin.tokens.empty() || ph.first != host.last + 1)
        continue;
      cue::Latin& h = host.latin;
      size_t k = h.tokens.size();
      while (k > 0 && (h.tokens[k - 1].features.pos == "verb" || text::latin_key(h.tokens[k - 1].text) == "non")) --k;
      // C24: a clause that is only its verb ("Est" + "in perīculō") takes the phrase first: "In perīculō est."
      if (k == h.tokens.size() || (k == 0 && h.tokens.size() > 1)) continue;
      cue::Latin head, tail, merged;
      const size_t cut = (size_t)h.tokens[k].start;
      head.text = h.text.substr(0, cut > 0 ? cut - 1 : 0);
      for (size_t t = 0; t < k; ++t) head.tokens.push_back(h.tokens[t]);
      tail.text = h.text.substr(cut);
      for (size_t t = k; t < h.tokens.size(); ++t) {
        rules::TokenView x = h.tokens[t];
        x.start -= (int)cut;
        x.end -= (int)cut;
        tail.tokens.push_back(x);
      }
      cue::Latin pl = ph.latin;
      std::vector<std::string> tx;
      for (const auto& t : pl.tokens) tx.push_back(t.text);
      decapitalise(tx[0]);
      rewriteTokens(pl, tx);
      decapitalise(pl.tokens[0].display);
      while (!pl.text.empty() && endsWithAny(pl.text, ",;:")) pl.text.pop_back();
      if (k == 0 && !tail.tokens.empty() &&
          !(tail.tokens[0].hasLemma && (la_->lemma(tail.tokens[0].lemmaId).flags & lex::ProperName))) {
        std::vector<std::string> tt;
        for (const auto& t : tail.tokens) tt.push_back(t.text);
        decapitalise(tt[0]);
        rewriteTokens(tail, tt);
        decapitalise(tail.tokens[0].display);
      }
      cue::append(merged, head);
      cue::append(merged, pl);
      cue::append(merged, tail);
      for (Reason& r : host.reasons)
        if (r.tokenIndex >= (int)k) r.tokenIndex += (int)pl.tokens.size();
      for (Reason r : ph.reasons) { if (r.tokenIndex >= 0) r.tokenIndex += (int)k; host.reasons.push_back(r); }
      host.sep = ph.sep;
      host.last = ph.last;
      h = merged;
      units.erase(units.begin() + (long)i);
      --i;
    }
    // C15: a connector phrase after a clause opens that clause ("..., however;" -> "Tamen ...;")
    for (size_t i = 1; i < units.size(); ++i)
      if (units[i].connFront) {
        const std::string after = units[i].sep;
        units[i].sep.clear();
        units[i - 1].sep = after;
        std::swap(units[i - 1], units[i]);
        units[i - 1].connFront = false;
      }
    // join the units with the source separators
    cue::Latin& L = so.latin;
    bool capNext = false;
    for (size_t i = 0; i < units.size(); ++i) {
      UnitText& ut = units[i];
      if (ut.latin.tokens.empty() && ut.latin.text.empty()) continue;
      std::vector<std::string> tx;
      for (const auto& t : ut.latin.tokens) tx.push_back(t.text);
      if (!tx.empty()) {
        if (L.text.empty() || capNext) capitalise(tx[0]);
        else if (!ut.nameFirst) decapitalise(tx[0]);
        rewriteTokens(ut.latin, tx);
        if (L.text.empty() || capNext) capitalise(ut.latin.tokens[0].display);
        else if (!ut.nameFirst) decapitalise(ut.latin.tokens[0].display);
      }
      capNext = false;
      const int base = (int)L.tokens.size();
      cue::append(L, ut.latin);
      // source offsets: the source word of the choice that produced each lemma, else the unit's start
      std::vector<char> usedChoice(so.choices.size(), 0);
      const size_t offFrom = so.srcOffset.size();
      std::vector<char> found;
      for (const rules::TokenView& t : ut.latin.tokens) {
        int off = ut.srcStart;
        bool hit = false;
        if (t.hasLemma)
          for (size_t k = ut.choiceFrom; k < ut.choiceTo; ++k)
            if (!usedChoice[k] && so.choices[k].lemma == t.lemmaId && so.choices[k].token >= 0 &&
                (size_t)so.choices[k].token < s.tokens.size()) {
              usedChoice[k] = 1;
              off = s.tokens[(size_t)so.choices[k].token].start;
              hit = true;
              break;
            }
        // C24: a name is found by its source spelling
        if (!hit && !t.text.empty() && (unsigned char)t.text[0] >= 'A' && (unsigned char)t.text[0] <= 'Z')
          for (int k = ut.first; k <= ut.last && k < (int)s.tokens.size(); ++k)
            if (k >= 0 && s.tokens[(size_t)k].upos == "PROPN" && !s.tokens[(size_t)k].text.empty() &&
                std::tolower((unsigned char)s.tokens[(size_t)k].text[0]) == std::tolower((unsigned char)t.text[0]) &&
                s.tokens[(size_t)k].text.size() >= 3 && t.text.size() >= 3 &&
                std::tolower((unsigned char)s.tokens[(size_t)k].text[1]) == std::tolower((unsigned char)t.text[1])) {
              off = s.tokens[(size_t)k].start;
              hit = true;
              break;
            }
        so.srcOffset.push_back(off);
        found.push_back(hit ? 1 : 0);
      }
      // C24: a word without a source word of its own (a pronoun, "et", a preposition) goes with its neighbour to the
      // right, else to the left (the cue split then keeps "cum Henrīcō" and a verb's clause together)
      if (s.tokens.size() > 0 && ut.last > ut.first) {
        std::vector<int> fill(found.size(), -1);
        for (size_t q = 0; q < found.size(); ++q) {
          if (found[q]) continue;
          for (size_t r = q + 1; r < found.size() && fill[q] < 0; ++r)
            if (found[r]) fill[q] = so.srcOffset[offFrom + r];
          for (size_t r = q; r > 0 && fill[q] < 0; --r)
            if (found[r - 1]) fill[q] = so.srcOffset[offFrom + r - 1];
        }
        for (size_t q = 0; q < found.size(); ++q)
          if (!found[q] && fill[q] >= 0) so.srcOffset[offFrom + q] = fill[q];
      }
      for (Reason r : ut.reasons) {
        if (r.tokenIndex >= 0) r.tokenIndex += base;
        so.reasons.push_back(r);
      }
      // a unit that ends with its own question / exclamation mark ("Quid?") takes no separator; the next is a
      // sentence of its own (C15)
      if (endsWithAny(L.text, "?!")) capNext = i + 1 < units.size();
      else if (i + 1 < units.size() && !ut.sep.empty()) L.text += ut.sep;
    }
    // C15 "never nonsense": the parse failed (no clause could hold the source verb) and the Latin has no verb: a
    // confident-looking sentence would be invented. The words are given one by one instead (dictionary forms,
    // unknown words in brackets), marked Fix with the reason "could not parse".
    // A statement of three or more content words without any verb, made only of fragments, while one of its nouns
    // can be read as a verb ("Colorless green ideas sleep furiously.") is a failed parse too.
    bool verblessStatement = false;
    if (st.lang == frame::SrcLang::En && en_ && (s.finalPunct == "." || s.finalPunct == "!")) {
      int content = 0;
      bool anyVerb = false, nounVerb = false, allFrag = !s.units.empty();
      for (const frame::Unit& u : s.units)
        if (u.type != frame::Unit::Clause || u.frame.type != frame::Kind::Frag || u.vocative) allFrag = false;
      for (size_t i = 0; i < s.tokens.size(); ++i) {
        const nlp::Token& t = s.tokens[i];
        if (t.upos == "VERB" || t.upos == "AUX") anyVerb = true;
        if (t.upos == "NOUN" || t.upos == "ADJ" || t.upos == "ADV" || t.upos == "PROPN") ++content;
        // C19: a noun inside a noun phrase (after a determiner, an adjective, a numeral or a possessive: "a very small
        // mouse with a long tail.") is no candidate verb of a failed parse
        const std::string& pu = i > 0 ? s.tokens[i - 1].upos : std::string();
        // C26: nor the head of a compound noun ("a silver cup", "a small brown mouse")
        if (t.upos == "NOUN" && i > 0 && pu != "DET" && pu != "ADJ" && pu != "NUM" && pu != "ADP" &&
            !(pu == "PRON" && s.tokens[i - 1].deprel == "nmod") &&
            !(pu == "NOUN" && s.tokens[i - 1].deprel == "compound" && s.tokens[i - 1].head == (int)i + 1 &&
              nlp::morph::get(s.tokens[i - 1].feats, nlp::morph::NumberShift) != nlp::morph::NumPlur &&
              !s.tokens[i - 1].lower.empty() && s.tokens[i - 1].lower.back() != 's')) {
          std::vector<lex::Analysis> an;
          en_->lookup(text::en_key(t.lower), an);
          for (const lex::Analysis& a : an) nounVerb = nounVerb || en_->lemma(a.lemma).pos == feat::Verb;
        }
      }
      verblessStatement = allFrag && !anyVerb && nounVerb && content >= 4;
    }
    if (((std::find(s.repairs.begin(), s.repairs.end(), "no-verb") != s.repairs.end()) || verblessStatement) &&
        st.lang == frame::SrcLang::En) {
      bool srcVerb = verblessStatement, laVerb = false;
      for (size_t i = 0; i < s.tokens.size(); ++i)
        if (s.tokens[i].upos == "VERB" && s.drop[i] == frame::Drop::No) srcVerb = true;
      for (const rules::TokenView& t : L.tokens) laVerb = laVerb || t.features.pos == "verb";
      // C24: before the word-by-word fallback, the sentence is cut into clauses at commas and conjunctions and each
      // clause translated on its own (Check, clause-split); used when at least one clause gives a Latin verb
      if (srcVerb && !laVerb && allowSplit) {
        const std::vector<size_t> pts = clausePoints(text);
        if (!pts.empty()) {
          transfer::Memory m2 = memEntry;
          SentOut alt;
          speechSplit(text, pts, fb, opt, ctx, m2, st, alt);
          bool verb = false;
          for (const rules::TokenView& t : alt.latin.tokens) verb = verb || t.features.pos == "verb";
          if (verb) {
            alt.flags.erase(std::remove(alt.flags.begin(), alt.flags.end(), std::string("frame-fallback")), alt.flags.end());
            addFlag(alt.flags, "clause-split");
            alt.reasons.push_back(Reason{-1, "form", "the sentence could not be analysed whole: it was translated clause by "
                                                     "clause; check how the clauses join", ""});
            so = std::move(alt);
            mem = m2;
            return;
          }
        }
      }
      if (srcVerb && !laVerb) {
        literalRender(s, st, so, covered);
        flags.push_back("could-not-parse");
      }
    }
    if (!s.repairs.empty()) {   // the analysis used a fallback: the structure may be wrong (Check)
      std::string what;
      for (const std::string& r : s.repairs) what += (what.empty() ? "" : ", ") + r;
      addFlag(flags, "frame-fallback");
      so.reasons.push_back(Reason{-1, "form", "sentence analysis fallback (" + what + "): check the structure", ""});
    }
    // C15: constructions rendered by a rule of thumb (frame.h SemSentence::doubts) and clause fragments cut from
    // their sentence are never OK (Check): the reader should look at them
    for (const std::string& d : s.doubts) {
      addFlag(flags, d);
      so.reasons.push_back(Reason{-1, "form", "construction rendered by a rule of thumb (" + d + "): check it", ""});
    }
    {
      const bool lowerStart = !text.empty() && text[0] >= 'a' && text[0] <= 'z';
      size_t e = text.size();
      while (e > 0 && text[e - 1] == ' ') --e;
      const bool openEnd = e > 0 && (text[e - 1] == ',' || text[e - 1] == ';' || text[e - 1] == ':' || text[e - 1] == '-');
      if (st.lang == frame::SrcLang::En && (lowerStart || openEnd)) {
        addFlag(flags, "fragment");
        so.reasons.push_back(Reason{-1, "form", "a clause cut from its sentence: check it in context", ""});
      }
    }
    std::string fp = s.finalPunct;
    if (fp.empty() && frame::endsSentence(text)) fp = ".";
    if (fp.empty() && !text.empty() && (text.back() == ',' || text.back() == ';')) fp = std::string(1, text.back());   // C15
    if (fp == "\xE2\x80\xA6") fp = "...";
    // C22: "Would you please sit down?" is said as a request: "Cōnsīde, quaesō."
    if (fp == "?" && std::find(s.doubts.begin(), s.doubts.end(), "polite-request") != s.doubts.end()) fp = ".";
    while (!L.text.empty() && endsWithAny(L.text, ",;:")) L.text.pop_back();
    L.text += fp;
    // C17: brackets back around the Latin of the editorial words (a contiguous run of tokens whose source word lies
    // inside the brackets); when the words are spread, no brackets and Check
    for (const auto& sp : edSpans) {
      addFlag(flags, "editorial");
      addFlag(so.flags, "editorial");
      long a1 = -1, b1 = -1;
      bool gap = false;
      for (size_t k = 0; k < L.tokens.size() && k < so.srcOffset.size(); ++k) {
        const int off = so.srcOffset[k];
        const bool inside = off > (int)sp.first && off < (int)sp.second;
        if (inside) {
          if (a1 < 0) a1 = (long)k;
          else if (b1 != (long)k - 1) gap = true;
          b1 = (long)k;
        }
      }
      if (a1 < 0 || gap) continue;
      const int ins0 = L.tokens[(size_t)a1].start;
      L.text.insert((size_t)ins0, "[");
      for (size_t k = (size_t)a1; k < L.tokens.size(); ++k) { L.tokens[k].start += 1; L.tokens[k].end += 1; }
      const int ins1 = L.tokens[(size_t)b1].end;
      L.text.insert((size_t)ins1, "]");
      for (size_t k = (size_t)b1 + 1; k < L.tokens.size(); ++k) { L.tokens[k].start += 1; L.tokens[k].end += 1; }
    }
    // C24: the quoted sound words back in place of the pronoun read for them
    // the Latin pronoun tokens (is, ea, id) in order; the n-th "it" of the parsed text is the n-th of them when the
    // counts agree
    std::vector<size_t> isTokens;
    std::vector<size_t> itWords;
    if (!soundSpans.empty()) {
      const uint32_t isL = xfer_->latin("is");
      for (size_t q = 0; q < L.tokens.size(); ++q)
        if (L.tokens[q].hasLemma && L.tokens[q].lemmaId == isL && isL != lex::kNoLemma) isTokens.push_back(q);
      for (size_t q = 0; q + 1 < parseText.size(); ++q)
        if ((parseText[q] == 'i' || parseText[q] == 'I') && parseText[q + 1] == 't' &&
            (q == 0 || !std::isalpha((unsigned char)parseText[q - 1])) &&
            (q + 2 >= parseText.size() || !std::isalpha((unsigned char)parseText[q + 2])))
          itWords.push_back(q);
    }
    for (const auto& sp : soundSpans) {
      const std::string quoted = text.substr(sp.first, sp.second - sp.first);
      long k = -1;
      for (size_t q = 0; q < L.tokens.size() && q < so.srcOffset.size(); ++q)
        if (so.srcOffset[q] >= (int)sp.first && so.srcOffset[q] < (int)sp.second) { k = (long)q; break; }
      if (k < 0 && isTokens.size() == itWords.size())
        for (size_t w = 0; w < itWords.size(); ++w)
          if (itWords[w] == sp.first) k = (long)isTokens[w];
      if (k >= 0) {
        std::vector<std::string> tx;
        for (const auto& t : L.tokens) tx.push_back(t.text);
        tx[(size_t)k] = quoted;
        rewriteTokens(L, tx);
        rules::TokenView& t = L.tokens[(size_t)k];
        t.display = quoted;
        t.hasLemma = false;
        t.lemmaId = 0;
        t.features = rules::Features{};
        t.tier = 0;
        t.emoji.clear();
        t.unknown = false;
      } else {
        // no place found: the quoted word before the final mark
        std::string fpx;
        while (!L.text.empty() && endsWithAny(L.text, ".!?")) { fpx.insert(fpx.begin(), L.text.back()); L.text.pop_back(); }
        cue::Latin one;
        rules::TokenView t;
        t.text = t.display = quoted;
        t.start = 0;
        t.end = (int)quoted.size();
        one.text = quoted;
        one.tokens.push_back(t);
        cue::append(L, one);
        so.srcOffset.push_back((int)sp.first);
        L.text += fpx;
      }
      for (size_t i = 0; i < s.tokens.size(); ++i)
        if (s.tokens[i].start >= (int)sp.first && s.tokens[i].start < (int)sp.second) covered.push_back((int)i);
      so.reasons.push_back(Reason{-1, "form", "a sound word in quotes is kept as it is written: " + quoted, ""});
    }
    for (const std::string& f : flags) addFlag(so.flags, f);
    // A7: source coverage
    std::sort(covered.begin(), covered.end());
    for (size_t i = 0; i < s.tokens.size(); ++i) {
      const nlp::Token& t = s.tokens[i];
      const bool content = t.upos == "NOUN" || t.upos == "PROPN" || t.upos == "VERB" || t.upos == "ADJ" ||
                           t.upos == "ADV" || t.upos == "NUM" || t.upos == "PRON";
      if (!content || s.drop[i] != frame::Drop::No) continue;
      if (std::binary_search(covered.begin(), covered.end(), (int)i)) continue;
      if (t.upos == "ADV" && (t.lower == "not" || t.lower == "n't")) continue;
      so.missing.push_back(t.text);
    }
    // margins
    for (const transfer::Choice& c : so.choices)
      if (c.kind == "sense" && c.candidates.size() > 1) so.minMargin = std::min(so.minMargin, c.margin);
    if (mem.sawFirst) mem.prevFirst = true;
    else mem.prevFirst = false;
    // alternatives: the second-best lemma of the most ambiguous word; the other speaker gender when unknown
    if (alternatives) {
      // C23 (D18): a phrasebook row with a twin of the other register (ecclesiastical / classical): the twin's
      // rendering ("I'm sorry": Habeās mē excūsātum chosen, Ignōsce mihi offered; "goodbye": Valē chosen, Deus tē
      // servet offered)
      for (const frame::Unit& u : s.units) {
        if (u.type != frame::Unit::Phrase) continue;
        const int twin = phraseTwin(u.phrase.entry, st.lang, st.classical);
        if (twin < 0) continue;
        const std::vector<curated::PhraseEntry>& book =
            st.lang == frame::SrcLang::Es ? cd_->phrasebookEs() : cd_->phrasebook();
        transfer::Memory m2 = memBefore_;
        transfer::Settings st2 = st;
        st2.phraseSwaps.push_back({u.phrase.entry, twin});
        SentOut alt;
        speech(text, fb, opt, ctx, m2, st2, alt, false);
        display(alt.latin, opt.macrons);
        if (alt.latin.text != so.latin.text)
          so.alternatives.push_back(Alternative{alt.latin.text,
                                                book[(size_t)twin].eccl ? "ecclesiastical rendering of \"" + u.phrase.pattern + "\""
                                                                        : "classical rendering of \"" + u.phrase.pattern + "\"",
                                                0.5});
        break;
      }
      const transfer::Choice* amb = nullptr;
      for (const transfer::Choice& c : so.choices)
        if (c.kind == "sense" && c.candidates.size() > 1 && c.token >= 0 && (!amb || c.margin < amb->margin)) amb = &c;
      if (amb && amb->margin < 0.3) {
        transfer::Memory m2 = memBefore_;
        transfer::Settings st2 = st;
        st2.overrides.push_back({amb->token, 1});
        SentOut alt;
        speech(text, fb, opt, ctx, m2, st2, alt, false);
        display(alt.latin, opt.macrons);
        if (alt.latin.text != so.latin.text) {
          const lex::Lemma l = la_->lemma(amb->candidates[1].lemma);
          so.alternatives.push_back(Alternative{alt.latin.text,
                                                "second sense of \"" + amb->source + "\": " + std::string(l.head),
                                                std::max(0.0, 1.0 - amb->margin)});
        }
      }
      const bool genderWords = std::find(so.flags.begin(), so.flags.end(), "speaker-gender") != so.flags.end() ||
                               firstPersonAgreement(so);
      if (opt.speakerGender == 'u' && genderWords) {
        transfer::Memory m2 = memBefore_;
        transfer::Settings st2 = st;
        st2.flipSpeakerGender = true;
        SentOut alt;
        speech(text, fb, opt, ctx, m2, st2, alt, false);
        display(alt.latin, opt.macrons);
        if (alt.latin.text != so.latin.text)
          so.alternatives.insert(so.alternatives.begin(), Alternative{alt.latin.text, "speaker: feminine", 0.5});
        addFlag(so.flags, "speaker-gender");
      }
    }
  }

  // Engine ii (closed choice): the most ambiguous word of the sentence, re-translated when the model prefers
  // another candidate. Its answer enters as a forced candidate rank, never as text.
  void askModel(const std::string& text, const frame::FrameBuilder& fb, const Options& opt, const Context& ctx,
                transfer::Memory& mem, const transfer::Settings& st, SentOut& so) {
    const transfer::Choice* amb = nullptr;
    for (const transfer::Choice& c : so.choices)
      if (c.kind == "sense" && c.candidates.size() > 1 && c.token >= 0 && (!amb || c.margin < amb->margin)) amb = &c;
    if (!amb) return;
    std::vector<std::string> options;
    for (const transfer::Candidate& k : amb->candidates) {
      const lex::Lemma l = la_->lemma(k.lemma);
      options.push_back(std::string(l.head) + " (" + std::string(l.glossEn) + ")");
    }
    int idx = -1;
    try {
      idx = cfg_.advisors.chooseSense("Which Latin word translates \"" + amb->source + "\" in: " + text, options);
    } catch (...) {
      idx = -1;
    }
    const std::string source = amb->source, chosen = idx >= 0 && (size_t)idx < options.size() ? options[(size_t)idx] : "";
    if (idx <= 0 || (size_t)idx >= amb->candidates.size()) {
      so.reasons.push_back(Reason{-1, "evidence", "model: keeps " + (options.empty() ? std::string() : options[0]), ""});
      return;
    }
    transfer::Memory m2 = memBefore_;
    transfer::Settings st2 = st;
    st2.overrides.push_back({amb->token, idx});
    SentOut alt;
    speech(text, fb, opt, ctx, m2, st2, alt, false);
    alt.alternatives = so.alternatives;
    cue::Latin first = so.latin;
    display(first, opt.macrons);
    alt.alternatives.insert(alt.alternatives.begin(), Alternative{first.text, "rule engine's first choice", 0.5});
    if (alt.alternatives.size() > 3) alt.alternatives.resize(3);
    alt.reasons.push_back(Reason{-1, "evidence", "model: chose " + chosen + " for \"" + source + "\"", ""});
    so = std::move(alt);
    mem = m2;
  }

  // C19 (work item b): a lexicon cell missing for the chosen lemma ("no lexicon cell for these features") is never left
  // as a bracketed Fix literal while another candidate of the same word has the form: the sentence is translated again
  // with the next candidate forced (at most three tries; the first result without a missing form wins).
  void retryMissingForms(const std::string& text, const frame::FrameBuilder& fb, const Options& opt, const Context& ctx,
                         transfer::Memory& mem, const transfer::Settings& st, SentOut& so) {
    auto missing = [](const SentOut& x) {
      return std::find(x.flags.begin(), x.flags.end(), "missing-form") != x.flags.end();
    };
    transfer::Settings st2 = st;
    for (int tries = 0; tries < 3 && missing(so); ++tries) {
      uint32_t bad = lex::kNoLemma;
      for (const Reason& r : so.reasons)
        if (r.kind == "form" && r.text == "no lexicon cell for these features" && r.tokenIndex >= 0 &&
            (size_t)r.tokenIndex < so.latin.tokens.size() && so.latin.tokens[(size_t)r.tokenIndex].hasLemma) {
          bad = so.latin.tokens[(size_t)r.tokenIndex].lemmaId;
          break;
        }
      if (bad == lex::kNoLemma) return;
      const transfer::Choice* ch = nullptr;
      for (const transfer::Choice& c : so.choices)
        if (c.lemma == bad && c.token >= 0 && c.candidates.size() > 1) { ch = &c; break; }
      if (!ch) return;
      size_t rank = 0;
      while (rank < ch->candidates.size() && ch->candidates[rank].lemma != bad) ++rank;
      if (rank + 1 >= ch->candidates.size()) return;
      const int token = ch->token;
      bool replaced = false;
      for (auto& ov : st2.overrides)
        if (ov.first == token) { ov.second = (int)rank + 1; replaced = true; }
      if (!replaced) st2.overrides.push_back({token, (int)rank + 1});
      transfer::Memory m2 = memBefore_;
      SentOut alt;
      speech(text, fb, opt, ctx, m2, st2, alt, false);
      if (missing(alt) && tries + 1 < 3) { so.alternatives = alt.alternatives; so = std::move(alt); mem = m2; continue; }
      if (!missing(alt)) {
        alt.reasons.push_back(Reason{-1, "form", "the first word had no form for this use: the next candidate was taken", ""});
        so = std::move(alt);
        mem = m2;
      }
      return;
    }
  }

  // C22: gender of the people named in a text (names_la.tsv rows with a gender, titles Miss / Mr included): F when a
  // feminine name is there, M when only masculine ones, 0 when none
  uint8_t namesGender(const std::string& t) const {
    if (!cd_) return 0;
    bool f = false, m = false;
    size_t a = 0;
    while (a < t.size()) {
      while (a < t.size() && !std::isalpha((unsigned char)t[a])) ++a;
      size_t b = a;
      while (b < t.size() && std::isalpha((unsigned char)t[b])) ++b;
      if (b > a && t[a] >= 'A' && t[a] <= 'Z') {
        const curated::NameEntry* ne = cd_->nameByEnglish(t.substr(a, b - a));
        if (ne && ne->gender == feat::F) f = true;
        else if (ne && ne->gender == feat::M) m = true;
      }
      a = b;
    }
    return f ? (uint8_t)feat::F : m ? (uint8_t)feat::M : (uint8_t)0;
  }

  static bool firstPersonAgreement(const SentOut& so) {
    for (const auto& t : so.latin.tokens)
      if ((t.features.pos == "adj" || t.features.mood == "participle") && !t.features.gender.empty()) return true;
    return false;
  }

  // ---- checks -------------------------------------------------------------------------------------------------------
  void runChecks(const std::string& target, const std::vector<rules::TokenView>& tokens, const Options& opt,
                 const CueInput& in, bool latinText, CueOutput& o, bool overflow, bool tagsApprox) {
    o.checks.clear();
    if (latinText) {
      check::Options co;
      co.tierCeiling = (uint8_t)(opt.fidelity >= 3 ? 1 : opt.fidelity == 2 ? 2 : 3);
      co.glossary = glossary_;
      std::vector<check::TokenHint> hints;
      for (const rules::TokenView& t : tokens) {
        if (t.start < 0) continue;
        check::TokenHint h;
        h.start = t.start;
        h.end = t.end;
        h.lemma = t.hasLemma ? t.lemmaId : lex::kNoLemma;
        h.fromRule = t.fromRule;
        h.name = !t.hasLemma && !t.unknown && !t.text.empty() && t.text[0] != '[';
        if (t.hasLemma && (la_->lemma(t.lemmaId).flags & lex::ProperName)) h.name = true;
        hints.push_back(h);
      }
      for (const Reason& r : o.reasons)
        if (r.kind == "name" && r.tokenIndex >= 0 && (size_t)r.tokenIndex < tokens.size())
          for (check::TokenHint& h : hints)
            if (h.start == tokens[(size_t)r.tokenIndex].start) h.name = true;
      co.hints = &hints;
      check::Report rep;
      checker_->check(target, co, rep);
      for (const Check& c : rep.checks) o.checks.push_back(c);
      if (rep.fromRule) addFlag(o.flags, "from-rule");
    } else {
      for (const char* id : {"A1", "A2", "A3", "A4", "A6"}) o.checks.push_back(Check{id, true, "not Latin text (copied)"});
    }
    // A5 markup / timing integrity
    {
      Check a5{"A5", true, ""};
      int sq = 0, rd = 0;
      for (char ch : target) {
        if (ch == '<' || ch == '{') { a5.ok = false; a5.detail = "stray markup in the text"; }
        if (ch == '[') ++sq;
        if (ch == ']') --sq;
        if (ch == '(') ++rd;
        if (ch == ')') --rd;
      }
      if (sq != 0 || rd != 0) { a5.ok = false; a5.detail = "unbalanced brackets"; }
      if (tagsApprox) {   // DESIGN 10.5: partial tags dropped, Check (not Fix)
        a5.detail += std::string(a5.ok ? "" : "; ") + "tag position approximated";
        a5.ok = false;
      }
      o.checks.push_back(a5);
    }
    // A7 is added by the caller (it knows the source), A8 here
    {
      Check a8{"A8", true, ""};
      double cps = 0;
      if (in.endMs > in.startMs) {
        std::string flat = target;
        std::replace(flat.begin(), flat.end(), '\n', ' ');
        cps = (double)subs::visibleLength(flat) / ((double)(in.endMs - in.startMs) / 1000.0);
      }
      if (cps > cfg_.cpsLimit) {
        a8.ok = false;
        a8.detail = "reading speed " + fmt(cps) + " cps > " + fmt(cfg_.cpsLimit);
        addFlag(o.flags, "cps");
      }
      if (overflow) {
        a8.ok = false;
        a8.detail += std::string(a8.detail.empty() ? "" : "; ") + "more than " + std::to_string(cfg_.maxLines) +
                     " lines of " + std::to_string(cfg_.maxLine);
        addFlag(o.flags, "overflow");
      }
      o.checks.push_back(a8);
    }
    o.checks.push_back(la2xA9(target, latinText, opt));   // C11 la2x hook
    std::stable_sort(o.checks.begin(), o.checks.end(), [](const Check& x, const Check& y) { return x.id < y.id; });
  }

  static bool checkOk(const CueOutput& o, const char* id) {
    for (const Check& c : o.checks)
      if (c.id == id) return c.ok;
    return true;
  }

  // ---- translate -----------------------------------------------------------------------------------------------------
  Result<std::vector<CueOutput>> translateImpl(const std::vector<CueInput>& cues, const Options& opt, const Context& ctx,
                                               const std::function<void(size_t)>& progress,
                                               const std::function<bool()>& cancelled) {
    std::vector<CueOutput> outs;
    if (cues.empty()) return Result<std::vector<CueOutput>>(std::move(outs));
    if (opt.target != Lang::La)
      return Result<std::vector<CueOutput>>(ErrorCode::BadParams, "only translation into Latin is implemented",
                                            "This language pair is not available yet.");
    if (opt.source != Lang::En && opt.source != Lang::Es)
      return Result<std::vector<CueOutput>>(ErrorCode::BadParams, "source must be English or Spanish",
                                            "This language pair is not available yet.");
    Result<void> r = ensureLatin();
    if (!r.ok()) return Result<std::vector<CueOutput>>(r.error());
    const frame::SrcLang lang = opt.source == Lang::Es ? frame::SrcLang::Es : frame::SrcLang::En;
    Result<const frame::FrameBuilder*> fbr = builder(lang);
    if (!fbr.ok()) return Result<std::vector<CueOutput>>(fbr.error());
    const frame::FrameBuilder& fb = *fbr.value();
    glossary_ = &ctx.glossary;

    // virtual neighbours: the previous cue (discourse memory, a sentence that continues into the first cue) and the
    // next cue when the last one does not end its sentence
    std::vector<std::string> texts;
    const bool prev = !cues.front().prevSource.empty();
    const bool next = !cues.back().nextSource.empty() && !frame::endsSentence(cues.back().sourceText);
    if (prev) texts.push_back(cues.front().prevSource);
    for (const CueInput& c : cues) texts.push_back(c.sourceText);
    if (next) texts.push_back(cues.back().nextSource);
    const size_t off = prev ? 1 : 0;
    std::vector<SourceSentence> sents = frame::mapSentences(texts);
    std::vector<char> cueSplit(sents.size(), 0);   // C22: re-split because a cue came out empty (Check)

    // which sentence finishes each cue
    std::vector<long> lastSentence(texts.size(), -1);
    auto mapLast = [&]() {
      std::fill(lastSentence.begin(), lastSentence.end(), -1);
      for (size_t si = 0; si < sents.size(); ++si)
        for (const frame::CuePart& p : sents[si].parts) lastSentence[p.cue] = (long)si;
    };
    mapLast();

    struct CueAcc {
      cue::Latin latin;
      std::vector<Reason> reasons;
      std::vector<std::string> flags, unknown, missing;
      std::vector<transfer::Choice> choices;
      double minMargin = 1.0;
      bool nonverbal = false, song = false, copied = false, latinText = false;
      int sentences = 0, wholeSentences = 0;
      std::vector<Alternative> alternatives;
    };
    std::vector<CueAcc> acc(texts.size());
    transfer::Memory mem;
    transfer::Settings st;
    st.lang = lang;
    st.fidelity = std::max(1, std::min(3, opt.fidelity));
    st.classical = opt.latinity == Latinity::Classical;   // C23 (D18)
    st.srcLex = lang == frame::SrcLang::Es ? es_ : en_;   // C13: English pivot of Spanish words
    st.speakerGender = opt.speakerGender == 'f' ? 'f' : opt.speakerGender == 'u' ? 'u' : 'm';
    st.context = &ctx;
    size_t reported = 0;
    size_t doneCues = 0;   // real cues finished
    bool stopped = false;
    long lastCueSeen = -1;
    // C24: per cue, the gender of the last person addressed and whether its last sentence asked for an answer
    std::vector<uint8_t> cueVoc(texts.size(), 0);
    std::vector<char> cueInv(texts.size(), 0);
    char turnHint = 0;
    std::string prevSongText;
    uint8_t lastSentVoc = 0;
    bool lastSentInv = false;
    for (size_t si = 0; si < sents.size(); ++si) {
      if (cancelled && cancelled()) { stopped = true; break; }
      const SourceSentence& ss = sents[si];
      // imperative number: a group addressed in the previous cue (imp.number)
      const long firstCue = ss.parts.empty() ? 0 : (long)ss.parts.front().cue;
      // C24: the speaker of a reply. A cue that answers a cue which addressed someone of known gender by name or title
      // and asked for an answer (a question, an order, the address alone: "Mr. Fox. Wait!") is spoken by that person;
      // a dash turn after such a turn in the same cue too. The project's speaker setting stays the default.
      if (firstCue != lastCueSeen) {
        turnHint = 0;
        if (firstCue > 0 && (size_t)firstCue - 1 < cueVoc.size() && cueInv[(size_t)firstCue - 1])
          turnHint = cueVoc[(size_t)firstCue - 1] == feat::F ? 'f' : cueVoc[(size_t)firstCue - 1] == feat::M ? 'm' : 0;
      } else if (ss.dash) {
        turnHint = lastSentInv ? (lastSentVoc == feat::F ? 'f' : lastSentVoc == feat::M ? 'm' : 0) : 0;
      }
      if (firstCue != lastCueSeen) {
        if (lastCueSeen >= 0) { mem.addresseePlural = mem.sawPlural; mem.sawPlural = false; }
        mem.addresseeGuess = false;
        lastCueSeen = firstCue;
      }
      // "you" number from the reply (decision 2): a sentence with "you" whose next sentence starts with "we" was
      // addressed to a group ("Why are you painting the roses?" - "We planted ...") -> plural, marked as a guess
      mem.answerWe = false;
      if (ss.kind == frame::CueKind::Speech && si + 1 < sents.size() && sents[si + 1].kind == frame::CueKind::Speech) {
        const std::string low = " " + text::lower(ss.text) + " ";
        const std::string nx = text::lower(sents[si + 1].text);
        bool you = false;
        for (const char* w : {" you ", " you?", " you.", " you!", " you,", " your "}) you = you || low.find(w) != std::string::npos;
        const bool we = nx.rfind("we ", 0) == 0 || nx.rfind("we'", 0) == 0 || nx.rfind("we\xE2\x80\x99", 0) == 0 ||
                        nx.rfind("nosotros ", 0) == 0;
        mem.answerWe = you && we && lang == frame::SrcLang::En;
        // Spanish (C13): a question whose next sentence answers with a 1st-person plural verb ("Plantamos ...") or
        // "nosotros" was put to "ustedes" (a 3rd-person plural verb without subject: 2nd plural in Latin)
        if (lang == frame::SrcLang::Es && frame::endsSentence(ss.text) && ss.text.find('?') != std::string::npos) {
          size_t a = 0;
          while (a < nx.size() && (nx[a] == '-' || nx[a] == ' ' || (unsigned char)nx[a] == 0xC2)) a += (unsigned char)nx[a] == 0xC2 ? 2 : 1;
          const size_t b = nx.find_first_of(" ,.!?", a);
          const std::string w = nx.substr(a, b == std::string::npos ? std::string::npos : b - a);
          mem.answerWe = w == "nosotros" || w == "nosotras" || (w.size() > 4 && w.compare(w.size() - 3, 3, "mos") == 0);
        }
      }
      SentOut so;
      // C19: a sentence cut at a comma continues in the next one of the same speaker (no speaker dash): its last object
      // case is offered to a fragment "and the queen."
      {
        const SourceSentence* pv = si > 0 ? &sents[si - 1] : nullptr;
        bool open = false;
        if (pv && pv->kind == frame::CueKind::Speech && !ss.dash) {
          size_t e = pv->text.size();
          while (e > 0 && pv->text[e - 1] == ' ') --e;
          open = e > 0 && (pv->text[e - 1] == ',' || pv->text[e - 1] == ';' || pv->text[e - 1] == ':' ||
                           pv->text[e - 1] == '-');
          // C26: a sentence that starts with a lower-case word continues the one before, whatever its last mark
          // ("I saw the king." + "and the queen." -> Et rēgīnam.)
          size_t a = 0;
          while (a < ss.text.size() && (ss.text[a] == ' ' || ss.text[a] == '"' || ss.text[a] == '\'')) ++a;
          if (a < ss.text.size() && ss.text[a] >= 'a' && ss.text[a] <= 'z') open = true;
        }
        mem.contCase = open ? mem.lastObjCase : 0;
      }
      // C22: the person addressed: a name of names_la.tsv in this cue, else in the previous cue ("Alice", "Dinah" ->
      // feminine: "puella cāra", "callida es")
      if (lang == frame::SrcLang::En) {
        uint8_t ag = firstCue >= 0 && (size_t)firstCue < texts.size() ? namesGender(texts[(size_t)firstCue]) : 0;
        if (!ag && firstCue > 0) ag = namesGender(texts[(size_t)firstCue - 1]);
        mem.addresseeGender = ag;
      }
      mem.songLine = ss.kind == frame::CueKind::Song;   // C22
      if (ss.kind == frame::CueKind::Nonverbal) mem.prevValid = false;
      memBefore_ = mem;
      if (ss.kind == frame::CueKind::Nonverbal) {
        bool translated = false;
        const std::string la = cue::nonverbal(ss.text, *cd_, translated);
        so.nonverbal = true;
        so.copied = !translated;
        so.latin.text = ss.prefix + la + ss.suffix;
        if (translated) {
          rules::TokenView t;
          tokenInfo(la, t);
          t.text = t.display = la;
          t.start = (int)ss.prefix.size();
          t.end = t.start + (int)la.size();
          so.latin.tokens.push_back(t);
          so.reasons.push_back(Reason{0, "phrasebook", "nonverbal: " + ss.text + " -> " + la, ""});
        }
        addFlag(so.flags, "nonverbal");
      } else {
        transfer::Settings sts = st;
        const bool hinted = turnHint && lang == frame::SrcLang::En && ss.kind == frame::CueKind::Speech;
        if (hinted) sts.speakerGender = turnHint;
        // C24: a song line that opens with a relative word ("That ...", "Where ...") after a line ending in a noun
        // without punctuation continues that line as its relative clause: it is analysed with the noun in front
        // ("a boat that carries me") and the noun's Latin word is taken out again ("quae mē ...")
        std::string antecedent;
        if (lang == frame::SrcLang::En && ss.kind == frame::CueKind::Song && en_) antecedent = songAntecedent(prevSongText, ss.text);
        if (!antecedent.empty()) {
          std::string t2 = ss.text;
          if (!t2.empty() && t2[0] >= 'A' && t2[0] <= 'Z') t2[0] = (char)(t2[0] - 'A' + 'a');
          std::string qmark;
          while (!t2.empty() && (t2.back() == '?' || t2.back() == ' ')) { if (t2.back() == '?') qmark = "?"; t2.pop_back(); }
          t2 = antecedent + " " + t2;
          speech(t2, fb, opt, ctx, mem, sts, so, true);
          if (!qmark.empty() && !so.latin.text.empty() && so.latin.text.back() == '.') so.latin.text.back() = '?';
          else if (!qmark.empty()) so.latin.text += qmark;
          const int plen = (int)antecedent.size();
          long cut = -1;
          for (size_t q = 0; q < so.latin.tokens.size() && q < so.srcOffset.size(); ++q)
            if (so.srcOffset[q] >= 0 && so.srcOffset[q] < plen && so.latin.tokens[q].features.pos == "noun") { cut = (long)q; break; }
          if (cut >= 0) {
            cue::Latin rest;
            const rules::TokenView& ct = so.latin.tokens[(size_t)cut];
            size_t from = (size_t)ct.end;
            while (from < so.latin.text.size() && so.latin.text[from] == ' ') ++from;
            rest.text = so.latin.text.substr(0, (size_t)ct.start) + so.latin.text.substr(from);
            for (size_t q = 0; q < so.latin.tokens.size(); ++q) {
              if ((long)q == cut) continue;
              rules::TokenView x = so.latin.tokens[q];
              if ((long)q > cut) { x.start -= (int)(from - (size_t)ct.start); x.end -= (int)(from - (size_t)ct.start); }
              rest.tokens.push_back(x);
            }
            std::vector<Reason> rs;
            for (Reason rr : so.reasons) {
              if (rr.tokenIndex == (int)cut) continue;
              if (rr.tokenIndex > (int)cut) --rr.tokenIndex;
              rs.push_back(rr);
            }
            so.reasons = rs;
            so.srcOffset.erase(so.srcOffset.begin() + cut);
            if (cut == 0 && !rest.tokens.empty()) {   // the line starts with the relative word: a capital
              std::vector<std::string> tx;
              for (const auto& t : rest.tokens) tx.push_back(t.text);
              capitalise(tx[0]);
              rewriteTokens(rest, tx);
              capitalise(rest.tokens[0].display);
            }
            so.latin = rest;
            addFlag(so.flags, "song-relative");
            so.reasons.push_back(Reason{-1, "form", "this song line is read as a relative clause of the line before it", ""});
          } else {
            so = SentOut{};
            mem = memBefore_;
            speech(ss.text, fb, opt, ctx, mem, sts, so, true);
          }
        } else {
          speech(ss.text, fb, opt, ctx, mem, sts, so, true);
        }
        retryMissingForms(ss.text, fb, opt, ctx, mem, sts, so);
        if (opt.useModel && cfg_.advisors.chooseSense) askModel(ss.text, fb, opt, ctx, mem, sts, so);
        // C24: the reply's speaker differs from the project setting and the Latin depends on it (a first-person
        // predicate): Check with the flag speaker-gender; the setting's rendering is offered as the alternative
        if (hinted && turnHint != st.speakerGender) {
          transfer::Memory m2 = memBefore_;
          SentOut other;
          speech(ss.text, fb, opt, ctx, m2, st, other, false);
          cue::Latin a = so.latin, b = other.latin;
          display(a, opt.macrons);
          display(b, opt.macrons);
          if (a.text != b.text) {
            addFlag(so.flags, "speaker-gender");
            addFlag(so.flags, "speaker-reply");
            so.reasons.push_back(Reason{-1, "form", std::string("the speaker of this reply is taken to be the person addressed "
                                                                "in the cue before (") + (turnHint == 'f' ? "feminine" : "masculine") +
                                                        "): check it", ""});
            so.alternatives.insert(so.alternatives.begin(), Alternative{b.text, std::string("speaker: ") +
                                                                       (st.speakerGender == 'f' ? "feminine" : st.speakerGender == 'm' ? "masculine" : "unknown") +
                                                                       " (project setting)", 0.5});
          }
        }
        display(so.latin, opt.macrons);
        if (ss.kind == frame::CueKind::Song) {
          so.song = true;
          addFlag(so.flags, "song");
          const int shift = (int)ss.prefix.size();
          so.latin.text = ss.prefix + so.latin.text + ss.suffix;
          for (auto& t : so.latin.tokens) { t.start += shift; t.end += shift; }
        }
      }
      // pieces back onto the cues
      if (so.srcOffset.size() != so.latin.tokens.size()) so.srcOffset.assign(so.latin.tokens.size(), -1);
      if (ss.kind == frame::CueKind::Song) so.srcOffset.assign(so.latin.tokens.size(), -1);
      const std::vector<cue::Latin> pieces = cue::splitSentence(ss, so.latin, &so.srcOffset);
      // C22: no cue may ever be emptied or swallowed by a neighbour. When a sentence spanning cues leaves one of them
      // without Latin words (its words were merged into another cue's piece), each cue's part is translated as a
      // sentence of its own (a fragment, Check).
      if (ss.kind == frame::CueKind::Speech && ss.parts.size() > 1) {
        bool empty = false;
        for (size_t p = 0; p < ss.parts.size(); ++p) {
          const std::string part = ss.text.substr((size_t)ss.parts[p].start,
                                                  (size_t)std::max(0, ss.parts[p].end - ss.parts[p].start));
          bool letters = false;
          for (char ch : part) letters = letters || (unsigned char)ch >= 0x80 || std::isalpha((unsigned char)ch);
          bool latinWords = false;
          if (p < pieces.size())
            for (char ch : pieces[p].text) latinWords = latinWords || (unsigned char)ch >= 0x80 || std::isalpha((unsigned char)ch);
          if (letters && !latinWords) empty = true;
        }
        if (empty) {
          std::vector<SourceSentence> one;
          for (size_t p = 0; p < ss.parts.size(); ++p) {
            SourceSentence x;
            x.kind = frame::CueKind::Speech;
            x.dash = ss.dash && p == 0;
            x.text = ss.text.substr((size_t)ss.parts[p].start, (size_t)std::max(0, ss.parts[p].end - ss.parts[p].start));
            x.parts.push_back(frame::CuePart{ss.parts[p].cue, 0, (int)x.text.size()});
            one.push_back(std::move(x));
          }
          const size_t n = one.size();
          sents.erase(sents.begin() + (long)si);
          sents.insert(sents.begin() + (long)si, std::make_move_iterator(one.begin()), std::make_move_iterator(one.end()));
          cueSplit.erase(cueSplit.begin() + (long)si);
          cueSplit.insert(cueSplit.begin() + (long)si, n, (char)1);
          mapLast();
          mem = memBefore_;
          --si;
          continue;
        }
      }
      if (cueSplit[si]) addFlag(so.flags, "cue-split");
      size_t tokBase = 0;
      for (size_t p = 0; p < pieces.size() && p < ss.parts.size(); ++p) {
        CueAcc& a = acc[ss.parts[p].cue];
        const int cueBase = (int)a.latin.tokens.size();
        cue::append(a.latin, pieces[p]);
        for (const Reason& rr : so.reasons) {
          if (rr.tokenIndex < 0) {
            if (p == 0) a.reasons.push_back(rr);
            continue;
          }
          if ((size_t)rr.tokenIndex >= tokBase && (size_t)rr.tokenIndex < tokBase + pieces[p].tokens.size()) {
            Reason x = rr;
            x.tokenIndex = cueBase + (int)((size_t)rr.tokenIndex - tokBase);
            a.reasons.push_back(x);
          }
        }
        tokBase += pieces[p].tokens.size();
        for (const std::string& f : so.flags) addFlag(a.flags, f);
        a.unknown.insert(a.unknown.end(), so.unknown.begin(), so.unknown.end());
        a.missing.insert(a.missing.end(), so.missing.begin(), so.missing.end());
        a.choices.insert(a.choices.end(), so.choices.begin(), so.choices.end());
        a.minMargin = std::min(a.minMargin, so.minMargin);
        a.nonverbal = a.nonverbal || so.nonverbal;
        a.song = a.song || so.song;
        a.copied = a.copied || so.copied;
        a.latinText = a.latinText || !so.copied;
        ++a.sentences;
        if (ss.parts.size() == 1) {
          ++a.wholeSentences;
          for (const Alternative& alt : so.alternatives) a.alternatives.push_back(alt);
        }
        if (mem.addresseeGuess) addFlag(a.flags, "addressee-guess");
      }
      prevSongText = ss.kind == frame::CueKind::Song ? ss.text : std::string();   // C24
      if (!ss.parts.empty() && ss.kind == frame::CueKind::Speech) {   // C24
        const size_t lc = ss.parts.back().cue;
        if (lc < cueVoc.size()) {
          if (so.vocGender) cueVoc[lc] = so.vocGender;
          cueInv[lc] = so.invites;
        }
        lastSentVoc = so.vocGender;
        lastSentInv = so.invites;
      } else {
        lastSentVoc = 0;
        lastSentInv = false;
      }
      // progress: real cues whose last sentence is done
      while (doneCues < cues.size() && lastSentence[off + doneCues] <= (long)si) ++doneCues;
      if (progress && doneCues > reported) {
        reported = doneCues;
        progress(doneCues);
      }
    }
    if (stopped) {
      // only cues that are complete
      size_t complete = 0;
      while (complete < cues.size() && lastSentence[off + complete] >= 0 &&
             (size_t)lastSentence[off + complete] < sents.size() && complete < doneCues)
        ++complete;
      doneCues = complete;
    } else {
      doneCues = cues.size();
    }
    outs.reserve(doneCues);
    for (size_t i = 0; i < doneCues; ++i) {
      CueAcc& a = acc[off + i];
      // a remembered whole-cue correction (key as the CLI stores it) replaces the translation
      const std::string ckey = lang == frame::SrcLang::Es ? text::es_key(cues[i].sourceText) : text::en_key(cues[i].sourceText);
      const Correction* corr = nullptr;
      for (const Correction& c : ctx.corrections)
        if (c.sourceKey == ckey && !c.target.empty()) corr = &c;
      if (corr) {
        CueOutput o = checkImpl(cues[i], corr->target, opt, ctx);
        glossary_ = &ctx.glossary;
        o.index = cues[i].index;
        const cue::Layout lay = cue::layout(corr->target, cfg_.maxLine, cfg_.maxLines);
        o.target = lay.joined;
        cue::relocate(o.target, o.tokens);
        o.reasons.push_back(Reason{-1, "correction", "your correction (" + corr->scope + ", used " +
                                                         std::to_string(corr->count) + " times)", corr->target});
        addFlag(o.flags, "correction");
        outs.push_back(std::move(o));
        continue;
      }
      CueOutput o;
      o.index = cues[i].index;
      const cue::Layout lay = cue::layout(a.latin.text, cfg_.maxLine, cfg_.maxLines);
      o.target = lay.joined;
      o.tokens = a.latin.tokens;
      cue::relocate(o.target, o.tokens);
      o.reasons = a.reasons;
      o.flags = a.flags;
      if (a.latin.text.empty() && !cues[i].sourceText.empty()) addFlag(o.flags, "merged");
      for (const auto& t : o.tokens)
        if (!t.emoji.empty()) { addFlag(o.flags, "emoji"); break; }
      if (std::find(o.flags.begin(), o.flags.end(), "name-guessed") != o.flags.end()) addFlag(o.flags, "unknownName");
      // reasons for the lexical choices: attached to the first token with that lemma
      std::vector<char> used(o.tokens.size(), 0);
      for (const transfer::Choice& c : a.choices) {
        int ti = -1;
        for (size_t k = 0; k < o.tokens.size(); ++k)
          if (!used[k] && o.tokens[k].hasLemma && o.tokens[k].lemmaId == c.lemma && c.lemma != lex::kNoLemma) {
            ti = (int)k;
            used[k] = 1;
            break;
          }
        if (c.lemma == lex::kNoLemma && !c.unknown) continue;
        const std::string head = c.lemma != lex::kNoLemma ? std::string(la_->lemma(c.lemma).head) : std::string();
        if (c.unknown) {
          o.reasons.push_back(Reason{ti, "sense", "\"" + c.source + "\": no Latin word in the dictionary (kept in brackets)", ""});
          continue;
        }
        std::string kind = c.kind == "table" ? "sense" : c.kind == "periphrasis" ? "sense" : c.kind;
        std::string txt = "\"" + c.source + "\" -> " + head;
        if (c.kind == "table") txt += " (closed-class table)";
        if (!c.note.empty()) txt += " (" + c.note + ")";
        if (c.kind == "sense" && !c.candidates.empty()) txt += " (score " + fmt(c.candidates[0].score) + ")";
        // C23 (D18): the chosen sense's register goes on its token (TokenView.register) and into the reason
        if (!c.registerTag.empty() && c.kind != "periphrasis") {
          txt += c.registerTag == "new-latin" ? " (New Latin sense)" : " (Medieval / Late Latin sense)";
          if (ti >= 0) o.tokens[(size_t)ti].registerTag = c.registerTag;
        }
        o.reasons.push_back(Reason{ti, kind, txt, ""});
        if (!c.candidates.empty()) {
          std::string data = "[";
          for (size_t k = 0; k < c.candidates.size(); ++k) {
            const lex::Lemma l = la_->lemma(c.candidates[k].lemma);
            if (k) data += ",";
            data += "{\"lemma\":" + std::to_string(c.candidates[k].lemma) + ",\"head\":\"" +
                    jsonEscape(std::string(l.head)) + "\",\"score\":" + fmt(c.candidates[k].score) + ",\"why\":\"" +
                    jsonEscape(c.candidates[k].why) + "\"}";
          }
          data += "]";
          o.reasons.push_back(Reason{ti, "candidate", "candidates for \"" + c.source + "\"", data});
        }
      }
      if (opt.useModel && !cfg_.advisors.chooseSense) o.reasons.push_back(Reason{-1, "evidence", "model: off", ""});
      if (opt.useOnline && !cfg_.advisors.onlineCheck) o.reasons.push_back(Reason{-1, "evidence", "online: off", ""});
      bool onlineDisagree = false;
      if (opt.useOnline && cfg_.advisors.onlineCheck) {
        for (const transfer::Choice& c : a.choices) {
          if (c.kind != "sense" || c.lemma == lex::kNoLemma) continue;
          const lex::Lemma l = la_->lemma(c.lemma);
          const Evidence ev = cfg_.advisors.onlineCheck(std::string(l.head), std::string(l.glossEn));
          o.reasons.push_back(Reason{-1, "evidence", ev.source + ": " + ev.detail, std::to_string(ev.verdict)});
          if (ev.verdict < 0) onlineDisagree = true;
        }
      }
      // tag policy on the source spans (CueInput.spans, filled by the CLI): a cue fully inside one tag pair keeps it,
      // position tags at the start ({\an8}) are kept, any other tag cannot be placed in the re-broken Latin
      bool tagsApprox = false;
      if (!cues[i].spans.empty()) {
        std::vector<subs::Span> sp;
        bool anyTag = false;
        for (const SpanIn& x : cues[i].spans) {
          subs::Span y;
          y.kind = x.tag ? subs::Span::Tag : subs::Span::Text;
          y.raw = x.raw;
          anyTag = anyTag || x.tag;
          sp.push_back(std::move(y));
        }
        const cue::TagResult tr = cue::applyTags(sp, o.target);
        tagsApprox = tr.approximated;
        if (tagsApprox) {
          addFlag(o.flags, "tags-approximated");
          o.reasons.push_back(Reason{-1, "form", "a tag inside the source text could not be placed in the Latin: check it", ""});
        } else if (anyTag) {
          addFlag(o.flags, "tags");
        }
      }
      la2xSources_ = la2xSourceLemmas(a.choices);   // C11 la2x hook
      runChecks(o.target, o.tokens, opt, cues[i], a.latinText && !a.copied, o, lay.overflow, tagsApprox);
      // A7 source coverage
      {
        Check a7{"A7", true, ""};
        std::vector<std::string> miss = a.missing;
        std::sort(miss.begin(), miss.end());
        miss.erase(std::unique(miss.begin(), miss.end()), miss.end());
        if (!miss.empty()) {
          a7.ok = false;
          a7.detail = "not accounted for:";
          for (const std::string& w : miss) a7.detail += " " + w;
        }
        o.checks.push_back(a7);
        std::stable_sort(o.checks.begin(), o.checks.end(), [](const Check& x, const Check& y) { return x.id < y.id; });
      }
      // confidence (§10.4)
      bool unknown = false;
      for (const auto& t : o.tokens) unknown = unknown || t.unknown;
      unknown = unknown || !a.unknown.empty();
      bool a5fix = false;   // A5 is a Fix unless the only finding is an approximated tag position (Check)
      for (const Check& k : o.checks)
        if (k.id == "A5" && !k.ok && k.detail != "tag position approximated") a5fix = true;
      const bool fix = !checkOk(o, "A1") || !checkOk(o, "A3") || !checkOk(o, "A4") || unknown || a5fix ||
                       std::find(o.flags.begin(), o.flags.end(), "could-not-parse") != o.flags.end();
      bool chk = !checkOk(o, "A5") || !checkOk(o, "A6") || !checkOk(o, "A7") || !checkOk(o, "A8") || !checkOk(o, "A9") ||
                 a.minMargin < 0.15 || a.song || a.nonverbal || onlineDisagree;
      for (const char* f : {"name-guessed", "from-rule", "addressee-guess", "missing-form", "merged", "frame-fallback",
                            "fragment", "contact-relative", "noun-infinitive", "purpose-guess", "light-verb",
                            "phrase-order", "participle-phrase", "ellipsis", "could-not-parse", "editorial",   // C17
                            "derived-word",   // C19
                            "cue-split", "addressee-gender",   // C22
                            "speaker-reply", "free-relative", "clause-split", "song-relative"})   // C24
        if (std::find(o.flags.begin(), o.flags.end(), f) != o.flags.end()) chk = true;
      // a tier 3 word chosen while a tier 1/2 word of the same sense existed (fidelity 1, a correction aside)
      for (const transfer::Choice& c : a.choices)
        if (c.lowTier) {
          chk = true;
          addFlag(o.flags, "low-tier");
          o.reasons.push_back(Reason{-1, "sense", "\"" + c.source + "\": a rarer word was chosen although a core word "
                                                  "of the same sense exists", ""});
          break;
        }
      if (opt.speakerGender == 'u' && std::find(o.flags.begin(), o.flags.end(), "speaker-gender") != o.flags.end())
        chk = true;
      // C23 (D18): latinity "classical" and the word chosen is still a Medieval / Late / New Latin sense (no classical
      // candidate won): Check with a hint. With "wide" the register is never a Check reason.
      if (opt.latinity == Latinity::Classical)
        for (const transfer::Choice& c : a.choices)
          if (c.kind == "sense" && !c.registerTag.empty() && c.lemma != lex::kNoLemma) {
            chk = true;
            addFlag(o.flags, "late-latin");
            o.reasons.push_back(Reason{-1, "sense", "\"" + c.source + "\" -> " + std::string(la_->lemma(c.lemma).head) +
                                                    ": only a " + (c.registerTag == "new-latin" ? "New" : "Medieval / Late") +
                                                    " Latin word was found, and classical Latin was asked for: check it",
                                       ""});
            break;
          }
      o.confidence = fix ? Confidence::Fix : chk ? Confidence::Check : Confidence::Ok;
      // score: product of per-choice confidences (sorting only)
      double score = 1.0;
      for (const transfer::Choice& c : a.choices)
        if (c.kind == "sense") score *= std::min(1.0, 0.6 + std::max(0.0, c.margin));
      if (fix) score *= 0.3;
      else if (chk) score *= 0.7;
      o.score = std::max(0.0, std::min(1.0, std::round(score * 1000) / 1000));
      // alternatives: only for cues made of whole sentences; the sentence text replaced in the cue text
      if (a.wholeSentences == a.sentences && a.sentences == 1)
        for (const Alternative& alt : a.alternatives) {
          if (o.alternatives.size() >= 3) break;
          Alternative x = alt;
          x.score = std::round(x.score * 1000) / 1000;
          o.alternatives.push_back(x);
        }
      outs.push_back(std::move(o));
    }
    if (progress && outs.size() > reported) progress(outs.size());
    glossary_ = nullptr;
    return Result<std::vector<CueOutput>>(std::move(outs));
  }

  // ---- check ---------------------------------------------------------------------------------------------------------
  CueOutput checkImpl(const CueInput& in, const std::string& target, const Options& opt, const Context& ctx) {
    glossary_ = &ctx.glossary;
    CueOutput o;
    o.index = in.index;
    o.target = target;
    check::Options co;
    co.tierCeiling = (uint8_t)(opt.fidelity >= 3 ? 1 : opt.fidelity == 2 ? 2 : 3);
    co.glossary = glossary_;
    check::Report rep;
    checker_->check(target, co, rep);
    for (const check::CheckedToken& ct : rep.tokens) {
      rules::TokenView t;
      t.text = ct.text;
      t.display = ct.text;
      t.start = ct.start;
      t.end = ct.end;
      t.fromRule = ct.fromRule;
      if (!ct.analysis.analyses.empty()) {
        const lex::Analysis& a = ct.analysis.analyses[0];
        t.lemmaId = a.lemma;
        t.hasLemma = true;
        t.features = realise::featureView(morph::packedOf(*la_, a));
        const lex::Lemma l = la_->lemma(a.lemma);
        t.tier = cd_->effectiveTier(l.key, l.pos, l.tier);
      } else if (!ct.name) {
        t.unknown = ct.analysis.unknown;
      }
      o.tokens.push_back(std::move(t));
    }
    bool overflow = false;
    {
      std::string flat = target;
      std::replace(flat.begin(), flat.end(), '\n', ' ');
      subs::breakLines(flat, cue::latinBreakHints(), cfg_.maxLine, cfg_.maxLines, &overflow);
      // the user's own line breaks count: more lines than allowed is an overflow too
      size_t lines = 1 + (size_t)std::count(target.begin(), target.end(), '\n');
      if (lines > (size_t)cfg_.maxLines) overflow = true;
    }
    for (const Check& c : rep.checks) (void)c;
    runChecks(target, o.tokens, opt, in, true, o, overflow, false);
    bool unknown = false;
    for (const auto& t : o.tokens) unknown = unknown || t.unknown;
    const bool fix = !checkOk(o, "A1") || !checkOk(o, "A3") || !checkOk(o, "A4") || !checkOk(o, "A5") || unknown;
    const bool chk = !checkOk(o, "A6") || !checkOk(o, "A8") || rep.fromRule;
    o.confidence = fix ? Confidence::Fix : chk ? Confidence::Check : Confidence::Ok;
    o.score = fix ? 0.3 : chk ? 0.7 : 1.0;
    glossary_ = nullptr;
    return o;
  }

  // ---- inspect -------------------------------------------------------------------------------------------------------
  Result<InspectResult> inspectImpl(const std::string& word, Lang lang, const Options& opt) {
    InspectResult res;
    auto fromLemma = [&](const lex::Lexicon& lx, uint32_t id, uint32_t packed, const std::string& disp) {
      Analysis a;
      const lex::Lemma l = lx.lemma(id);
      a.lemmaId = id;
      a.head = std::string(l.head);
      a.glossEn = std::string(l.glossEn);
      a.glossEs = std::string(l.glossEs);
      a.features = realise::featureView(packed);
      a.display = disp;
      a.tier = l.tier;
      return a;
    };
    if (lang == Lang::La || lang == Lang::Grc) {
      const lex::Lexicon* lx = lang == Lang::La ? la_ : grc_;
      if (!lx)
        return Result<InspectResult>(ErrorCode::LexiconMissing, "no lexicon",
                                     "The dictionary for this language is not installed.");
      morph::Token t;
      if (lang == Lang::La) morph::analyseLatin(*lx, word, t);
      else morph::analyseGreek(*lx, word, t);
      for (const lex::Analysis& a : t.analyses)
        res.analyses.push_back(fromLemma(*lx, a.lemma, morph::packedOf(*lx, a),
                                         a.display.empty() ? word : std::string(a.display)));
      for (const morph::RuleAnalysis& a : t.ruleAnalyses) res.analyses.push_back(fromLemma(*lx, a.lemma, a.packed, a.display));
      if (res.analyses.empty()) {
        const std::string key = lang == Lang::La ? text::latin_key(word) : text::greek_key(word);
        std::vector<std::string_view> keys;
        for (size_t n = key.size(); n >= 2 && keys.empty(); --n) {
          if (n < key.size() && ((unsigned char)key[n] & 0xC0) == 0x80) continue;
          lx->prefix(std::string_view(key).substr(0, n), 5, keys);
        }
        for (std::string_view k : keys) res.suggestions.emplace_back(k);
      }
      return Result<InspectResult>(std::move(res));
    }
    // English / Spanish: the source lemma(s), then the Latin candidates (display = Latin headword)
    const lex::Lexicon* src = lang == Lang::En ? en_ : es_;
    if (!src)
      return Result<InspectResult>(ErrorCode::LexiconMissing, "no source lexicon",
                                   "The dictionary for this language is not installed.");
    const std::string low = text::lower(word);
    std::vector<lex::Analysis> an;
    src->lookup(lang == Lang::En ? text::en_key(low) : text::es_key(low), an);
    std::vector<std::string> lemmas;
    for (const lex::Analysis& a : an) {
      const lex::Lemma l = src->lemma(a.lemma);
      res.analyses.push_back(fromLemma(*src, a.lemma, src->feature(a.feat), word));
      const std::string h = text::lower(l.head);
      if (std::find(lemmas.begin(), lemmas.end(), h) == lemmas.end()) lemmas.push_back(h);
      if (res.analyses.size() >= 12) break;
    }
    if (lemmas.empty()) lemmas.push_back(low);
    if (la_ && ensureLatin().ok()) {
      transfer::Settings st;
      st.lang = lang == Lang::Es ? frame::SrcLang::Es : frame::SrcLang::En;
      st.fidelity = std::max(1, std::min(3, opt.fidelity));
      st.classical = opt.latinity == Latinity::Classical;   // C23
      size_t added = 0;
      for (const std::string& lm : lemmas) {
        for (uint8_t pos : {feat::Noun, feat::Verb, feat::Adj, feat::Adv}) {
          transfer::Choice ch;
          xfer_->select(lm, pos, {}, false, false, st, ch);
          for (const transfer::Candidate& c : ch.candidates) {
            if (added >= 12) break;
            const lex::Lemma l = la_->lemma(c.lemma);
            feat::Features f;
            f.pos = l.pos;
            Analysis a = fromLemma(*la_, c.lemma, feat::pack(f), std::string(l.head));
            res.analyses.push_back(a);
            ++added;
          }
        }
      }
    }
    if (res.analyses.empty()) {
      std::vector<std::string_view> keys;
      src->prefix(low.substr(0, std::min<size_t>(low.size(), 3)), 5, keys);
      for (std::string_view k : keys) res.suggestions.emplace_back(k);
    }
    return Result<InspectResult>(std::move(res));
  }

  EngineConfig cfg_;
  std::mutex m_;
  const lex::Lexicon* la_ = nullptr;
  const lex::Lexicon* grc_ = nullptr;
  const lex::Lexicon* en_ = nullptr;
  const lex::Lexicon* es_ = nullptr;
  std::unique_ptr<curated::CuratedData> cd_;
  std::unique_ptr<realise::LatinRealiser> realiser_;
  std::unique_ptr<check::LatinChecker> checker_;
  std::unique_ptr<transfer::Transfer> xfer_;
  std::unique_ptr<nlp::Pipeline> nlpEn_, nlpEs_;
  std::unique_ptr<frame::FrameBuilder> fbEn_, fbEs_;
  const std::vector<GlossaryEntry>* glossary_ = nullptr;
  transfer::Memory memBefore_;

  // ==== C12 grc (Greek engine path: en-grc, es-grc, grc-en, grc-es): BEGIN ==========================================
  // Owned by task C12 (engine/rules/src/{transfer_grc,grc2x,engine_grc}). Reached only through the one-line hooks
  // marked "C12 grc hook": the four Greek pairs in translate(), check() of a Greek cue (target or source Greek), and
  // inspect() of Greek words and of English / Spanish words when the target is Greek.
  std::unique_ptr<grc::GreekPath> grc2_;
  const lex::Lexicon* grc2Lex_ = nullptr;

  static bool grcPair(const Options& o) { return grc::GreekPath::toGreek(o) || grc::GreekPath::fromGreek(o); }
  static bool grcInspects(Lang lang, const Options& o) {
    return lang == Lang::Grc || ((lang == Lang::En || lang == Lang::Es) && o.target == Lang::Grc);
  }
  grc::GreekPath* grcPath(Error* err) {
    if (!grc_) {
      if (err) *err = Error{ErrorCode::LexiconMissing, "no Greek lexicon", "The Greek dictionary (greek.vpl) is not installed."};
      return nullptr;
    }
    Result<void> c = ensureCurated();
    if (!c.ok()) {
      if (err) *err = c.error();
      return nullptr;
    }
    if (!grc2_ || grc2Lex_ != grc_) {
      grc2_.reset();
      const std::vector<stdfs::path> dirs = {cfg_.curatedDir, stdfs::path(cfg_.dataDir) / "curated",
                                             stdfs::path(cfg_.dataDir) / ".." / "curated",
                                             stdfs::path(cfg_.dataDir) / ".." / ".." / "data" / "curated"};
      Result<std::unique_ptr<grc::GreekPath>> r =
          grc::GreekPath::create(*grc_, *cd_, dirs, grc::PathConfig{cfg_.cpsLimit, cfg_.maxLine, cfg_.maxLines});
      if (!r.ok()) {
        if (err) *err = r.error();
        return nullptr;
      }
      grc2_ = std::move(r.value());
      grc2Lex_ = grc_;
    }
    return grc2_.get();
  }
  Result<std::vector<CueOutput>> grcTranslate(const std::vector<CueInput>& cues, const Options& opt, const Context& ctx,
                                              const std::function<void(size_t)>& progress,
                                              const std::function<bool()>& cancelled) {
    Error err{ErrorCode::Internal, "", ""};
    grc::GreekPath* p = grcPath(&err);
    if (!p) return Result<std::vector<CueOutput>>(err);
    if (grc::GreekPath::toGreek(opt)) {   // the source models open lazily; a missing model is reported by the path
      const frame::SrcLang lang = opt.source == Lang::Es ? frame::SrcLang::Es : frame::SrcLang::En;
      Result<const frame::FrameBuilder*> b = builder(lang);
      if (!b.ok()) return Result<std::vector<CueOutput>>(b.error());
    }
    p->setSources(nlpEn_.get(), en_, nlpEs_.get(), es_);
    return p->translate(cues, opt, ctx, progress, cancelled);
  }
  Result<CueOutput> grcCheck(const CueInput& cue, const std::string& target, const Options& opt, const Context& ctx) {
    Error err{ErrorCode::Internal, "", ""};
    grc::GreekPath* p = grcPath(&err);
    if (!p) return Result<CueOutput>(err);
    if (grc::GreekPath::fromGreek(opt)) {   // the edited text is English / Spanish: only markup and reading speed
      CueOutput o;
      o.index = cue.index;
      o.target = target;
      o.confidence = Confidence::Check;
      o.score = 0.7;
      o.checks.push_back(Check{"A1", true, "not Greek text"});
      return Result<CueOutput>(std::move(o));
    }
    return Result<CueOutput>(p->check(cue, target, opt, ctx));
  }
  Result<InspectResult> grcInspect(const std::string& word, Lang lang, const Options& opt) {
    Error err{ErrorCode::Internal, "", ""};
    grc::GreekPath* p = grcPath(&err);
    if (!p) return Result<InspectResult>(err);
    p->setSources(nlpEn_.get(), en_, nlpEs_.get(), es_);
    return p->inspect(word, lang, opt);
  }
  // ==== C12 grc: END ================================================================================================

  // ==== C11 la2x (Latin -> English / Spanish; A9 round trip): BEGIN =================================================
  // Owned by task C11 (engine/rules/src/la2x). The rest of the engine reaches it only through the one-line hooks
  // marked "C11 la2x hook": pairs la-en / la-es in translate(), Latin glosses in inspect(), A9 in runChecks() with the
  // source lemmas set just before runChecks() in translateImpl().
  std::unique_ptr<la2x::Translator> la2x_;
  const lex::Lexicon* la2xLex_ = nullptr;
  std::vector<std::string> la2xSources_;   // EN/ES content lemmas of the cue being checked (consumed by la2xA9)
  bool la2xSourcesSet_ = false;            // set by the translateImpl hook; check() of an edited cue has no source

  static bool la2xPair(const Options& o) {
    return o.source == Lang::La && (o.target == Lang::En || o.target == Lang::Es);
  }
  la2x::Translator* la2xTranslator(Error* err) {
    if (!la_) {
      if (err) *err = Error{ErrorCode::LexiconMissing, "no Latin lexicon", "The Latin dictionary (latin.vpl) is not installed."};
      return nullptr;
    }
    Result<void> c = ensureCurated();
    if (!c.ok()) {
      if (err) *err = c.error();
      return nullptr;
    }
    if (la2x_ && la2xLex_ == la_) return la2x_.get();
    la2x_.reset();
    const std::vector<stdfs::path> dirs = {cfg_.curatedDir, stdfs::path(cfg_.dataDir) / "curated",
                                           stdfs::path(cfg_.dataDir) / ".." / "curated",
                                           stdfs::path(cfg_.dataDir) / ".." / ".." / "data" / "curated"};
    Result<std::unique_ptr<la2x::Translator>> r = la2x::Translator::create(*la_, *cd_, dirs);
    if (!r.ok()) {
      if (err) *err = r.error();
      return nullptr;
    }
    la2x_ = std::move(r.value());
    la2xLex_ = la_;
    return la2x_.get();
  }
  Result<std::vector<CueOutput>> la2xTranslate(const std::vector<CueInput>& cues, const Options& opt, const Context& ctx,
                                               const std::function<void(size_t)>& progress,
                                               const std::function<bool()>& cancelled) {
    Error err{ErrorCode::Internal, "", ""};
    la2x::Translator* t = la2xTranslator(&err);
    if (!t) return Result<std::vector<CueOutput>>(err);
    return Result<std::vector<CueOutput>>(t->cues(cues, opt, ctx, progress, cancelled));
  }
  // word.inspect for Latin: the glosses from the curated tables first (readable_*.tsv, gloss_es_la.tsv)
  Result<InspectResult> la2xInspect(Result<InspectResult> r, Lang lang, const Options& opt) {
    (void)opt;
    if (lang != Lang::La || !r.ok()) return r;
    la2x::Translator* t = la2xTranslator(nullptr);
    if (!t) return r;
    for (Analysis& a : r.value().analyses) {
      if (a.lemmaId == lex::kNoLemma || a.lemmaId >= la_->lemmaCount()) continue;
      const std::string en = t->gloss(a.lemmaId, la2x::Target::En);
      const std::string es = t->gloss(a.lemmaId, la2x::Target::Es);
      if (!en.empty()) a.glossEn = en;
      if (!es.empty()) a.glossEs = es;
    }
    return r;
  }
  std::vector<std::string> la2xSourceLemmas(const std::vector<transfer::Choice>& choices) {
    la2xSourcesSet_ = true;
    std::vector<std::string> out;
    for (const transfer::Choice& c : choices)
      if (c.kind != "table" && !c.source.empty()) out.push_back(c.source);
    return out;
  }
  Check la2xA9(const std::string& target, bool latinText, const Options& opt) {
    std::vector<std::string> src;
    src.swap(la2xSources_);
    const bool set = la2xSourcesSet_;
    la2xSourcesSet_ = false;
    if (!set) return Check{"A9", true, "not implemented for edited cues (no source lemmas in check())"};
    if (!latinText || opt.target != Lang::La) return Check{"A9", true, "not Latin text"};
    if (src.empty()) return Check{"A9", true, "no source content words"};
    la2x::Translator* t = la2xTranslator(nullptr);
    if (!t) return Check{"A9", true, "round trip not available"};
    std::string flat = target;
    std::replace(flat.begin(), flat.end(), '\n', ' ');
    const double ov = t->roundTripOverlap(flat, src, opt.source == Lang::Es ? la2x::Target::Es : la2x::Target::En);
    Check c{"A9", ov >= 0.5, "round-trip overlap " + fmt(ov)};
    return c;
  }
  // ==== C11 la2x: END ===============================================================================================

  // ==== C14 orberg (Orbergise, pair la-la): BEGIN ===================================================================
  // Owned by task C14 (engine/rules/src/orberg). Reached only through the one-line hooks marked "C14 orberg hook":
  // the include and the dispatch of pair la-la (Options.orbergise) in translate(). The original-language path reuses
  // this engine's EN/ES -> LA pipeline (frame builder, transfer at fidelity 3, realiser) through
  // orberg::EngineContext::fromOriginal.
  std::unique_ptr<orberg::Resources> orbergRes_;
  const lex::Lexicon* orbergLex_ = nullptr;

  static bool orbergPair(const Options& o) { return o.source == Lang::La && o.target == Lang::La; }

  Result<std::vector<CueOutput>> orbergTranslate(const std::vector<CueInput>& cues, const Options& opt,
                                                 const Context& ctx, const std::function<void(size_t)>& progress,
                                                 const std::function<bool()>& cancelled) {
    Result<void> r = ensureLatin();
    if (!r.ok()) return Result<std::vector<CueOutput>>(r.error());
    Error err{ErrorCode::Internal, "", ""};
    la2x::Translator* t = la2xTranslator(&err);
    if (!t) return Result<std::vector<CueOutput>>(err);
    if (!orbergRes_ || orbergLex_ != la_) {
      orbergRes_.reset();
      const std::vector<stdfs::path> dirs = {cfg_.curatedDir, stdfs::path(cfg_.dataDir) / "curated",
                                             stdfs::path(cfg_.dataDir) / ".." / "curated",
                                             stdfs::path(cfg_.dataDir) / ".." / ".." / "data" / "curated"};
      Result<std::unique_ptr<orberg::Resources>> rr = orberg::Resources::create(*la_, *cd_, dirs);
      if (!rr.ok()) return Result<std::vector<CueOutput>>(rr.error());
      orbergRes_ = std::move(rr.value());
      orbergLex_ = la_;
    }
    orberg::EngineContext ec;
    ec.la = la_;
    ec.cd = cd_.get();
    ec.la2x = t;
    ec.checker = checker_.get();
    ec.resources = orbergRes_.get();
    ec.glossary = &ctx.glossary;
    ec.cpsLimit = cfg_.cpsLimit;
    ec.maxLine = cfg_.maxLine;
    ec.maxLines = cfg_.maxLines;
    ec.fromOriginal = [this, &opt, &ctx](const std::string& text, Lang lang, int ceiling,
                                         const std::vector<uint32_t>& prefer, orberg::OriginalLatin& out) {
      return orbergFromOriginal(text, lang, ceiling, prefer, opt, ctx, out);
    };
    t->resetDiscourse();
    std::vector<CueOutput> outs = orberg::cues(cues, opt, ctx, ec, progress, cancelled);
    glossary_ = nullptr;
    return Result<std::vector<CueOutput>>(std::move(outs));
  }

  // The original-language cue through this engine's EN/ES -> LA pipeline at fidelity 3 (tier 1 preferred, periphrasis
  // allowed); a candidate that is one of the input's own core lemmas (`prefer`) is forced, so words the student
  // already reads stay.
  bool orbergFromOriginal(const std::string& text, Lang lang, int ceiling, const std::vector<uint32_t>& prefer,
                          const Options& opt0, const Context& ctx, orberg::OriginalLatin& out) {
    (void)ceiling;
    if (lang != Lang::En && lang != Lang::Es) return false;
    const frame::SrcLang sl = lang == Lang::Es ? frame::SrcLang::Es : frame::SrcLang::En;
    Result<const frame::FrameBuilder*> fbr = builder(sl);
    if (!fbr.ok()) return false;
    const frame::FrameBuilder& fb = *fbr.value();
    Options opt = opt0;
    opt.source = lang;
    opt.target = Lang::La;
    opt.fidelity = 3;
    opt.orbergise = false;
    opt.useModel = false;
    opt.useOnline = false;
    opt.emoji = false;
    transfer::Settings st;
    st.lang = sl;
    st.fidelity = 3;
    st.classical = opt.latinity == Latinity::Classical;   // C23
    st.srcLex = sl == frame::SrcLang::Es ? es_ : en_;
    st.speakerGender = opt.speakerGender == 'f' ? 'f' : opt.speakerGender == 'u' ? 'u' : 'm';
    st.context = &ctx;
    glossary_ = &ctx.glossary;
    transfer::Memory mem;
    std::string all;
    for (const SourceSentence& ss : frame::mapSentences({text})) {
      if (ss.kind != frame::CueKind::Speech) { glossary_ = nullptr; return false; }
      const transfer::Memory m0 = mem;
      memBefore_ = mem;
      SentOut so;
      speech(ss.text, fb, opt, ctx, mem, st, so, false);
      transfer::Settings st2 = st;
      for (const transfer::Choice& c : so.choices) {
        if (c.kind != "sense" || c.token < 0 || std::binary_search(prefer.begin(), prefer.end(), c.lemma)) continue;
        for (size_t k = 1; k < c.candidates.size(); ++k)
          if (std::binary_search(prefer.begin(), prefer.end(), c.candidates[k].lemma)) {
            st2.overrides.push_back({c.token, (int)k});
            break;
          }
      }
      if (!st2.overrides.empty()) {
        transfer::Memory m2 = m0;
        SentOut s2;
        speech(ss.text, fb, opt, ctx, m2, st2, s2, false);
        so = std::move(s2);
        mem = m2;
      }
      display(so.latin, opt.macrons);
      if (!so.unknown.empty()) out.unknown = true;
      if (std::find(so.flags.begin(), so.flags.end(), "frame-fallback") != so.flags.end()) out.fallback = true;
      for (const transfer::Choice& c : so.choices)
        if (c.lemma != lex::kNoLemma && c.kind != "table") out.lemmas.push_back(c.lemma);
      if (!all.empty()) all += ' ';
      all += so.latin.text;
    }
    glossary_ = nullptr;
    out.text = all;
    return !all.empty();
  }
  // ==== C14 orberg: END =============================================================================================
};

}  // namespace

std::unique_ptr<Engine> makeEngine(const EngineConfig& config) { return std::make_unique<RulesEngine>(config); }
std::unique_ptr<Engine> makeEngine() { return std::make_unique<RulesEngine>(defaultEngineConfig()); }

}  // namespace vp::rules
