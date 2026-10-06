// Engine wiring (C8): the rules engine built from EngineConfig with the data folders next to the lexicons, the
// language pairs it can serve, the advisors of engine ii (local model) and engine iii (online check) for the running
// job, and the per-cue engine data (tokens, flags, job facts) the views need.
#include <algorithm>
#include <cmath>

#include "locate.h"
#include "online_mock.h"
#include "server.h"
#include "vp/engine_config.h"
#include "vp/fs.h"
#include "vp/text.h"

namespace vpcli {

namespace fs = vp::fs;
using vp::ErrorCode;

namespace {
const char* const kPairs[] = {"en-la", "es-la", "la-en", "la-es", "en-grc", "es-grc", "grc-en", "grc-es", "la-la"};
}  // namespace

void Server::setupEngine() {
  onlineMock_ = envU8("VP_ONLINE_MOCK");
  if (onlineMock_ == "0") onlineMock_.clear();
  nlpDir_ = findNlpDir(opt_.lexiconDir);
  curatedDir_ = findCuratedDir(opt_.lexiconDir);
  nlpEn_ = nlpModelsPresent(nlpDir_, "english");
  nlpEs_ = nlpModelsPresent(nlpDir_, "spanish");
  samples_ = installSamples(opt_.dataDir);
  // tier counts of the Latin and Greek lexicons (engine.hello.lexicons[].tiers; the Engines tab's effect line)
  for (size_t i = 0; i < 2; ++i) {
    if (!lexOk_[i]) continue;
    uint32_t t[4] = {0, 0, 0, 0};
    const uint32_t n = lex_[i].lemmaCount();
    for (uint32_t id = 0; id < n; ++id) {
      const uint8_t tier = lex_[i].lemma(id).tier;
      if (tier >= 1 && tier <= 3) ++t[tier];
    }
    lexInfo_[i]["tiers"] = json{{"t1", t[1]}, {"t2", t[2]}, {"t3", t[3]}};
  }
  bool stub = opt_.stub;
#if !defined(VP_HAVE_RULES)
  stub = true;
#endif
  if (stub) {
    engine_ = vp::rules::makeStubEngine();
  } else {
#if defined(VP_HAVE_RULES)
    vp::rules::EngineConfig cfg;
    cfg.dataDir = opt_.lexiconDir;
    cfg.curatedDir = curatedDir_;
    cfg.nlpDir = nlpDir_.empty() ? fs::join(opt_.lexiconDir, "nlp") : nlpDir_;
    cfg.cpsLimit = cpsLimit_;
    cfg.advisors.chooseSense = [this](const std::string& prompt, const std::vector<std::string>& options) {
      return adviseSense(prompt, options);
    };
    cfg.advisors.onlineCheck = [this](const std::string& lemma, const std::string& gloss) {
      return adviseOnline(lemma, gloss);
    };
    engine_ = vp::rules::makeEngine(cfg);
    realEngine_ = true;
#endif
  }
  vp::Result<void> lr = engine_->setLexicons(lexOk_[0] ? &lex_[0] : nullptr, lexOk_[1] ? &lex_[1] : nullptr,
                                             lexOk_[2] ? &lex_[2] : nullptr, lexOk_[3] ? &lex_[3] : nullptr);
  if (!lr) logMsg(LogLevel::Warn, "engine lexicons: " + lr.error().message);
  logMsg(LogLevel::Info, std::string("engine ") + engine_->version() + (realEngine_ ? "" : " (stub)") + ", nlp " +
                             (nlpDir_.empty() ? std::string("(none)") : nlpDir_) + ", curated " +
                             (curatedDir_.empty() ? std::string("(none)") : curatedDir_) +
                             (onlineMock_.empty() ? "" : ", online MOCK transport (tests)"));
}

const std::vector<PairState>& Server::pairStates() {
  if (pairsProbed_) return pairs_;
  pairsProbed_ = true;
  pairs_.clear();
  for (const char* p : kPairs) {
    PairState st;
    st.pair = p;
    PairLangs pl;
    parsePair(p, pl);
    const bool intoLatinFromSource = pl.target == vp::rules::Lang::La &&
                                     (pl.source == vp::rules::Lang::En || pl.source == vp::rules::Lang::Es);
    if (!realEngine_) {
      st.available = true;
    } else if (!lexFor(pl.target) || (pl.source == vp::rules::Lang::La || pl.source == vp::rules::Lang::Grc ? !lexFor(pl.source) : false)) {
      const bool greek = pl.target == vp::rules::Lang::Grc || pl.source == vp::rules::Lang::Grc;
      st.why = vp::Error{ErrorCode::LexiconMissing, std::string(greek && !lexOk_[1] ? "greek.vpl" : "latin.vpl") +
                                                        " not found in " + opt_.lexiconDir,
                         greek && !lexOk_[1] ? "The Greek dictionary (greek.vpl) is not installed."
                                             : "The Latin dictionary (latin.vpl) is not installed. Reinstall the app."};
    } else if (intoLatinFromSource) {
      const bool en = pl.source == vp::rules::Lang::En;
      const std::string base = en ? "english" : "spanish";
      if (curatedDir_.empty()) {
        st.why = vp::Error{ErrorCode::NotFound, "curated rule tables (order_la.txt) not found",
                           "The rule tables of the translator (data/curated) are missing. Reinstall the app."};
      } else if (!(en ? nlpEn_ : nlpEs_)) {
        st.why = vp::Error{ErrorCode::NotFound,
                           base + ".tag.vpt / " + base + ".dep.vpt not found" + (nlpDir_.empty() ? "" : " in " + nlpDir_),
                           std::string("The ") + (en ? "English" : "Spanish") + " language analysis files (" + base +
                               ".tag.vpt, " + base + ".dep.vpt) are missing from the data folder (data/nlp). "
                               "Reinstall the app."};
      } else {
        st.available = true;
      }
    } else {
      // Pairs the rules engine may or may not implement yet: ask it with one empty cue (cheap: unimplemented pairs
      // are refused before any work).
      vp::rules::CueInput c;
      vp::rules::Options o;
      o.source = pl.source;
      o.target = pl.target;
      o.orbergise = pl.source == pl.target;
      vp::rules::Context ctx;
      vp::Result<std::vector<vp::rules::CueOutput>> r =
          engine_->translate({c}, o, ctx, std::function<void(size_t)>(), std::function<bool()>());
      if (r) st.available = true;
      else st.why = r.error();
    }
    pairs_.push_back(std::move(st));
  }
  return pairs_;
}

void Server::requirePair(const std::string& pair) {
  for (const PairState& st : pairStates())
    if (st.pair == pair) {
      if (!st.available) throw CmdError{st.why};
      return;
    }
}

bool Server::orbergMode() const { return project_.manifest.pair == "la-la"; }

// ---- engine ii: closed choices, only inside a job that asked for the model ----
int Server::adviseSense(const std::string& prompt, const std::vector<std::string>& options) {
  if (!jobModel_.chooseSense || jobModelFailed_ || options.empty()) return -1;
  vp::Result<int> r = jobModel_.chooseSense(prompt, options, nullptr);
  if (!r) {
    jobModelFailed_ = true;   // one warning per job; the rule engine keeps its own choices
    jobWarning("model", r.error());
    return -1;
  }
  const int idx = r.value();
  if (idx >= 0 && static_cast<size_t>(idx) < options.size()) {
    std::string head = options[static_cast<size_t>(idx)];
    const size_t paren = head.find(" (");
    if (paren != std::string::npos) head.resize(paren);
    if (std::find(batchFacts_.modelHeads.begin(), batchFacts_.modelHeads.end(), head) == batchFacts_.modelHeads.end())
      batchFacts_.modelHeads.push_back(head);
  }
  return idx;
}

// ---- engine iii: evidence only, only when the job asked for it and the settings allow it at call time ----
vp::rules::Evidence Server::adviseOnline(const std::string& lemma, const std::string& gloss) {
  vp::rules::Evidence ev;
  ev.source = "online";
  if (!jobOnline_ || jobOnlineFailed_ || !vp::online::onlineAllowed(settings_)) {
    ev.detail = lemma + ": off";
    return ev;
  }
  if (!onlineClient_) {
    vp::online::Config cfg;
    cfg.cacheDir = fs::join(opt_.dataDir, "online-cache");
    if (!onlineMock_.empty()) {
      onlineTransport_ = makeMockTransport(lexOk_[0] ? &lex_[0] : nullptr, onlineMock_ == "disagree", &mockCalls_);
      mockThrottle_ = std::make_unique<vp::online::Throttle>(1);
      cfg.throttle = mockThrottle_.get();
    } else {
      vp::Result<std::unique_ptr<vp::online::Transport>> t = vp::online::makeSystemTransport(settings_);
      if (!t) {
        jobOnlineFailed_ = true;
        jobWarning("online", t.error());
        ev.detail = lemma + ": unavailable";
        return ev;
      }
      onlineTransport_ = std::move(t.value());
    }
    vp::Result<std::unique_ptr<vp::online::Wiktionary>> c = vp::online::Wiktionary::create(*onlineTransport_, settings_, cfg);
    if (!c) {
      jobOnlineFailed_ = true;
      jobWarning("online", c.error());
      ev.detail = lemma + ": unavailable";
      return ev;
    }
    onlineClient_ = std::move(c.value());
  }
  const char* lang = jobTarget_ == vp::rules::Lang::Grc ? "grc" : "la";
  const vp::online::Evidence e = vp::online::makeOnlineCheck(*onlineClient_)(lang, lemma, gloss);
  using V = vp::online::Evidence;
  ev.verdict = e.verdict == V::Agrees ? 1 : e.verdict == V::Disagrees ? -1 : 0;
  ev.detail = lemma + ": " + e.summary + (e.url.empty() ? "" : " " + e.url);
  if (e.verdict == V::Error) {
    jobOnlineFailed_ = true;   // offline or blocked: stop asking for the rest of the job
    jobWarning("online", vp::Error{ErrorCode::OnlineFailed, e.summary,
                                   "Could not reach wiktionary.org. The translation goes on without the online check."});
    return ev;
  }
  batchFacts_.onlineVerdicts.emplace_back(lemma, ev.verdict);
  return ev;
}

void Server::jobWarning(const char* engine, const vp::Error& e) {
  logMsg(LogLevel::Warn, std::string("job ") + std::to_string(jobId_) + " " + engine + ": " + e.message);
  sendEvent("translate.warning", json{{"jobId", jobId_}, {"engine", engine}, {"code", vp::errorCodeName(e.code)},
                                      {"message", e.message}, {"hint", e.hint}});
}

// ---- per-cue engine data ----
std::vector<vp::rules::TokenView> Server::tokensOf(size_t pos) const {
  const vp::CueRecord& r = project_.cues[pos];
  std::vector<vp::rules::TokenView> t;
  if (const vp::CueReason* h = hiddenReason(r, kTokensKind))
    if (decodeTokens(h->data, t)) return t;
  t.clear();
  if (!r.target.empty()) {
    vp::Result<vp::rules::CueOutput> out = engine_->check(cueInput(pos), r.target, engineOptions(), engineContext());
    if (out) t = out->tokens;
  }
  return t;
}

void Server::retarget(size_t pos, const std::string& text) {
  vp::CueRecord& r = project_.cues[pos];
  const std::vector<vp::rules::TokenView> before = tokensOf(pos);
  std::vector<std::string> flags;
  for (const std::string& f : storedFlags(r))
    if (f == "song" || f == "nonverbal") flags.push_back(f);   // properties of the source cue
  r.target = text;
  std::vector<vp::rules::TokenView> now;
  vp::Result<vp::rules::CueOutput> chk = engine_->check(cueInput(pos), text, engineOptions(), engineContext());
  if (chk) {
    r.confidence = confidenceName(chk->confidence);
    r.score = std::isfinite(chk->score) ? std::min(1.0, std::max(0.0, chk->score)) : 0.0;
    r.checks.clear();
    for (const auto& c : chk->checks) r.checks.push_back(vp::CueCheck{c.id, c.ok, c.detail});
    now = chk->tokens;
    flags.insert(flags.end(), chk->flags.begin(), chk->flags.end());
  } else {
    logMsg(LogLevel::Warn, "check after edit: " + chk.error().message);
  }
  r.reasons = remapReasons(r.reasons, before, now);
  setHidden(r, kTokensKind, now.empty() ? std::string() : encodeTokens(now));
  setStoredFlags(r, flags);
}

void Server::storeOutput(size_t pos, const vp::rules::CueOutput& out) {
  vp::CueRecord& rec = project_.cues[pos];
  applyOutput(out, rec);
  setHidden(rec, kJobKind, encodeJob(batchFacts_));
}

std::vector<size_t> Server::indicesParam(const json& p, bool* given) const {
  std::vector<size_t> positions;
  const json* idx = param(p, "indices");
  if (given) *given = idx != nullptr;
  if (!idx) return positions;
  if (!idx->is_array()) fail(ErrorCode::BadParams, "'indices' must be an array", "The request was malformed.");
  for (const json& v : *idx) {
    if (!v.is_number_integer() || v.get<int64_t>() < 0 || v.get<int64_t>() >= static_cast<int64_t>(project_.cues.size()))
      fail(ErrorCode::BadParams, "bad cue index in 'indices'", "A cue number was out of range.");
    positions.push_back(static_cast<size_t>(v.get<int64_t>()));
  }
  std::sort(positions.begin(), positions.end());
  positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
  return positions;
}

// ---- Orbergise: the original-language file ----
void Server::loadOriginal(const std::string& path) {
  vp::subs::Format f;
  if (!formatFromPath(path, f))
    fail(ErrorCode::UnsupportedFormat, "unknown file type: " + path, "Only .srt, .vtt, .ass, .ssa and .txt files can be opened.");
  const std::string bytes = unwrap(fs::readFile(path, 64ull << 20));
  std::vector<uint8_t> b(bytes.begin(), bytes.end());
  const vp::subs::Document d = unwrap(vp::subs::parse(b, f));
  std::vector<std::string> out(project_.cues.size());
  if (d.cues.size() == doc_.cues.size() || d.format == vp::subs::Format::Txt || format_ == vp::subs::Format::Txt) {
    for (size_t i = 0; i < out.size() && i < d.cues.size(); ++i) out[i] = d.cues[i].plainText();
  } else {
    // different cue counts: the original cue(s) overlapping each cue in time
    std::vector<std::pair<int64_t, int64_t>> ot(d.cues.size());
    for (size_t j = 0; j < d.cues.size(); ++j) vp::subs::parseTiming(d.cues[j].timingRaw, d.format, ot[j].first, ot[j].second);
    for (size_t i = 0; i < out.size(); ++i) {
      int64_t s = 0, e = 0;
      if (!vp::subs::parseTiming(doc_.cues[i].timingRaw, format_, s, e)) continue;
      for (size_t j = 0; j < d.cues.size(); ++j) {
        const int64_t ov = std::min(e, ot[j].second) - std::max(s, ot[j].first);
        if (ov > 0 && ov * 2 >= std::min(e - s, ot[j].second - ot[j].first)) {
          if (!out[i].empty()) out[i] += ' ';
          out[i] += d.cues[j].plainText();
        }
      }
    }
  }
  originals_ = std::move(out);
  originalPath_ = path;
  project_.manifest.settingsSnapshot["orbergOriginalPath"] = path;
}

}  // namespace vpcli
