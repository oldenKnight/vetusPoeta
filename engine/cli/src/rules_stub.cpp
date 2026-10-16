// Stub of vp::rules::Engine used until engine/rules lands (DESIGN §9, rules.h): translate copies the source text,
// marks every cue Check with the reason "stub engine" and tokenises on spaces; check runs A5 (markup/line layout)
// only; inspect answers from the Latin/Greek (or English/Spanish) lexicon when one was handed in, so the Word
// inspector already shows real analyses. VP_STUB_DELAY_US (tests only) slows translate down per cue.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <memory>
#include <thread>

#include "views.h"
#include "vp/lex.h"
#include "vp/rules.h"
#include "vp/text.h"

namespace vp::rules {
namespace {

std::vector<TokenView> spaceTokens(const std::string& text) {
  std::vector<TokenView> out;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t')) ++i;
    if (i >= text.size()) break;
    size_t j = i;
    while (j < text.size() && text[j] != ' ' && text[j] != '\n' && text[j] != '\r' && text[j] != '\t') ++j;
    TokenView t;
    t.text = text.substr(i, j - i);
    t.display = t.text;
    t.start = static_cast<int>(i);
    t.end = static_cast<int>(j);
    t.unknown = false;
    out.push_back(std::move(t));
    i = j;
  }
  return out;
}

Reason stubReason() {
  Reason r;
  r.kind = "evidence";
  r.text = "stub engine";
  return r;
}

class StubEngine final : public Engine {
 public:
  StubEngine() {
    const char* d = std::getenv("VP_STUB_DELAY_US");
    if (d) delayUs_ = std::max(0, std::min(100000, std::atoi(d)));
  }

  Result<void> setLexicons(const lex::Lexicon* latin, const lex::Lexicon* greek, const lex::Lexicon* english,
                           const lex::Lexicon* spanish) override {
    la_ = latin;
    grc_ = greek;
    en_ = english;
    es_ = spanish;
    return Result<void>();
  }

  Result<std::vector<CueOutput>> translate(const std::vector<CueInput>& cues, const Options& opt, const Context&,
                                           const std::function<void(size_t)>& progress,
                                           const std::function<bool()>& cancelled) override {
    try {
      std::vector<CueOutput> out;
      out.reserve(cues.size());
      for (size_t i = 0; i < cues.size(); ++i) {
        if (cancelled && cancelled()) break;
        if (delayUs_ > 0) std::this_thread::sleep_for(std::chrono::microseconds(delayUs_));
        out.push_back(make(cues[i].index, cues[i].sourceText, opt));
        if (progress) progress(i + 1);
      }
      return Result<std::vector<CueOutput>>(std::move(out));
    } catch (const std::exception& e) {
      return Result<std::vector<CueOutput>>(ErrorCode::Internal, std::string("stub translate: ") + e.what(),
                                            "The translation stopped because of an internal error.");
    }
  }

  Result<CueOutput> check(const CueInput& cue, const std::string& target, const Options& opt, const Context&) override {
    try {
      return Result<CueOutput>(make(cue.index, target, opt));
    } catch (const std::exception& e) {
      return Result<CueOutput>(ErrorCode::Internal, std::string("stub check: ") + e.what(),
                               "The cue could not be checked.");
    }
  }

  Result<InspectResult> inspect(const std::string& word, Lang lang, const Options&) override {
    try {
      const lex::Lexicon* lx = lang == Lang::La ? la_ : lang == Lang::Grc ? grc_ : lang == Lang::En ? en_ : es_;
      if (!lx)
        return Result<InspectResult>(ErrorCode::LexiconMissing,
                                     std::string("no lexicon for '") + vpcli::langCode(lang) + "'",
                                     "The dictionary for this language is not installed.");
      InspectResult r;
      const std::string key = lang == Lang::Grc ? vpcli::lexKey(word, true)
                              : lang == Lang::La ? vpcli::lexKey(word, false)
                                                 : vp::text::lower(word);
      std::vector<lex::Analysis> an;
      lx->lookup(key, an);
      for (const lex::Analysis& a : an) {
        const lex::Lemma l = lx->lemma(a.lemma);
        Analysis x;
        x.lemmaId = a.lemma;
        x.head = std::string(l.head);
        x.glossEn = std::string(l.glossEn);
        x.glossEs = std::string(l.glossEs);
        x.features = vpcli::featuresFromPacked(lx->feature(a.feat));
        x.display = a.display.empty() ? word : std::string(a.display);
        x.tier = l.tier;
        r.analyses.push_back(std::move(x));
      }
      if (r.analyses.empty() && !key.empty()) {
        std::vector<std::string_view> keys;
        for (size_t n = key.size(); n >= 2 && keys.empty(); --n) {
          if (n < key.size() && (static_cast<unsigned char>(key[n]) & 0xC0) == 0x80) continue;   // UTF-8 boundary
          lx->prefix(std::string_view(key).substr(0, n), 5, keys);
        }
        for (std::string_view k : keys) r.suggestions.emplace_back(k);
      }
      return Result<InspectResult>(std::move(r));
    } catch (const std::exception& e) {
      return Result<InspectResult>(ErrorCode::Internal, std::string("stub inspect: ") + e.what(),
                                   "The word could not be looked up.");
    }
  }

  std::string version() const override { return "stub-1"; }

 private:
  static CueOutput make(uint32_t index, const std::string& text, const Options&) {
    CueOutput o;
    o.index = index;
    o.target = text;
    o.confidence = Confidence::Check;
    o.score = 0.5;
    o.tokens = spaceTokens(text);
    o.checks.push_back(vpcli::markupCheck(text, 42, 2));
    o.reasons.push_back(stubReason());
    return o;
  }

  const lex::Lexicon* la_ = nullptr;
  const lex::Lexicon* grc_ = nullptr;
  const lex::Lexicon* en_ = nullptr;
  const lex::Lexicon* es_ = nullptr;
  int delayUs_ = 0;
};

}  // namespace

std::unique_ptr<Engine> makeStubEngine() { return std::make_unique<StubEngine>(); }

}  // namespace vp::rules
