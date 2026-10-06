// Cue commands, translate/orbergise jobs, history, names, corrections, words and eval.
#include <algorithm>
#include <cmath>
#include <map>

#include "server.h"
#include "vp/fs.h"
#include "vp/online.h"
#include "vp/text.h"

namespace vpcli {

namespace fs = vp::fs;
using vp::ErrorCode;

CueViewCtx Server::viewCtx() const {
  CueViewCtx c;
  c.format = format_;
  c.cpsLimit = cpsLimit_;
  return c;
}

json Server::cueView(size_t pos) const { return cueViewJson(doc_.cues[pos], sources_[pos], project_.cues[pos], viewCtx()); }

vp::rules::Options Server::engineOptions() const {
  const json s = settings_.get();
  vp::rules::Options o;
  o.source = langs_.source;
  o.target = langs_.target;
  o.fidelity = s.value("defaultFidelity", 2);
  o.emoji = s.value("showEmoji", true);
  o.macrons = s.value("showMacrons", true);
  // speaker gender of first-person predicates (rules_la_notes decision 1): "m" | "f" | "u"; default "m"
  const std::string g = s.contains("speakerGender") && s["speakerGender"].is_string() ? s["speakerGender"].get<std::string>() : "m";
  o.speakerGender = g == "f" ? 'f' : (g == "u" || g == "unknown") ? 'u' : 'm';
  return o;
}

vp::rules::Context Server::engineContext() const {
  vp::rules::Context c;
  for (const vp::NameEntry& n : project_.glossary) {
    vp::rules::GlossaryEntry g;
    g.name = n.name;
    g.policy = n.policy;
    g.form = n.form;
    g.gender = n.gender;
    try {
      g.declension = n.declension.empty() ? 0 : std::stoi(n.declension);
    } catch (...) {
      g.declension = 0;
    }
    c.glossary.push_back(std::move(g));
  }
  for (const vp::Correction& x : project_.corrections)
    c.corrections.push_back(vp::rules::Correction{x.key, x.target, x.scope, x.count});
  return c;
}

vp::rules::CueInput Server::cueInput(size_t pos) const {
  vp::rules::CueInput in;
  in.index = static_cast<uint32_t>(pos);
  in.sourceText = sources_[pos];
  int64_t s = 0, e = 0;
  if (format_ != vp::subs::Format::Txt && vp::subs::parseTiming(doc_.cues[pos].timingRaw, format_, s, e)) {
    in.startMs = s;
    in.endMs = e;
  }
  if (pos > 0) in.prevSource = sources_[pos - 1];
  if (pos + 1 < sources_.size()) in.nextSource = sources_[pos + 1];
  // the source cue's text and tag spans, so the cue assembly can keep {\an8} / a whole-cue <i> and report tags it
  // cannot place (A5)
  for (const vp::subs::Span& sp : doc_.cues[pos].spans) {
    vp::rules::SpanIn x;
    x.tag = sp.kind == vp::subs::Span::Tag;
    x.raw = sp.kind == vp::subs::Span::Newline ? std::string("\n") : sp.raw;
    in.spans.push_back(std::move(x));
  }
  return in;
}

void Server::dropOldestStep() {
  std::deque<HistoryStep>& q = undo_.empty() ? redo_ : undo_;
  if (q.empty()) return;
  historyRecords_ -= std::min(historyRecords_, q.front().before.size() + q.front().after.size());
  q.pop_front();
}

void Server::pushHistory(HistoryStep&& step) {
  if (step.before.empty()) return;
  for (const HistoryStep& r : redo_) historyRecords_ -= std::min(historyRecords_, r.before.size() + r.after.size());
  redo_.clear();
  historyRecords_ += step.before.size() + step.after.size();
  undo_.push_back(std::move(step));
  while (undo_.size() > kHistoryCap || (historyRecords_ > kHistoryRecordCap && undo_.size() > 1)) dropOldestStep();
}

namespace {
std::string stateAfterUnreview(const vp::CueRecord& r) {
  if (r.edited) return "edited";
  return r.target.empty() ? "new" : "translated";
}

std::string sourceKey(vp::rules::Lang l, const std::string& s) {
  switch (l) {
    case vp::rules::Lang::La: return vp::text::latin_key(s);
    case vp::rules::Lang::Grc: return vp::text::greek_key(s);
    case vp::rules::Lang::Es: return vp::text::es_key(s);
    default: return vp::text::en_key(s);
  }
}

bool isWordByte(unsigned char c) { return c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

bool containsWord(const std::string& text, const std::string& word) {
  if (word.empty()) return false;
  for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
    const bool left = at == 0 || !isWordByte(static_cast<unsigned char>(text[at - 1]));
    const size_t end = at + word.size();
    const bool right = end >= text.size() || !isWordByte(static_cast<unsigned char>(text[end]));
    if (left && right) return true;
  }
  return false;
}
}  // namespace

// ---------------------------------------------------------------------------------------------- cue.*
json Server::cmdCuePage(const json& p) {
  requireProject();
  const int64_t total = static_cast<int64_t>(project_.cues.size());
  const int64_t from = intParam(p, "from", 0, 0, std::max<int64_t>(total, 0));
  const int64_t count = intParam(p, "count", 50, 0, 1000000);
  const int64_t end = std::min(total, from + std::min<int64_t>(count, 200));
  json cues = json::array();
  for (int64_t i = from; i < end; ++i) cues.push_back(cueView(static_cast<size_t>(i)));
  return json{{"total", total}, {"cues", std::move(cues)}};
}

json Server::cmdCueGet(const json& p) {
  const size_t pos = position(p);
  const vp::CueRecord& r = project_.cues[pos];
  json alts = json::array(), tokens = json::array(), checks = json::array();
  for (const vp::Alternative& a : r.alternatives) alts.push_back(altJson(a));
  for (const vp::CueCheck& c : r.checks) checks.push_back(checkJson(c));
  // the tokens the engine produced (stored with the cue; re-analysed by Engine::check when absent), so reasons'
  // tokenIndex points into this list
  const std::vector<vp::rules::TokenView> toks = r.target.empty() ? std::vector<vp::rules::TokenView>() : tokensOf(pos);
  for (const vp::rules::TokenView& t : toks) tokens.push_back(tokenJson(t));
  ReasonViewCtx rc;
  rc.lex = lexFor(langs_.target);
  rc.latin = langs_.target == vp::rules::Lang::La;
  rc.source = sources_[pos];
  json out{{"cue", cueView(pos)}, {"alternatives", alts}, {"tokens", tokens}, {"checks", checks},
           {"reasons", reasonsView(r, toks, rc)}};
  JobFacts job;
  const vp::CueReason* jh = hiddenReason(r, kJobKind);
  const bool orberg = orbergMode() || (jh && decodeJob(jh->data, job) && job.orberg);
  if (orberg) {   // Orbergise mode: meaning check against the input Latin, the original-language line
    if (!r.target.empty() && lexFor(vp::rules::Lang::La)) {
      const Meaning m = meaningCheck(*lexFor(vp::rules::Lang::La), sources_[pos], r.target);
      out["meaning"] = json{{"percent", m.percent}, {"missing", m.missing}};
    }
    if (pos < originals_.size()) out["original"] = originals_[pos];
  }
  return out;
}

json Server::cmdCueSet(const json& p) {
  const size_t pos = position(p);
  const std::string text = strParam(p, "text", "", true);
  if (text.size() > 64 * 1024) fail(ErrorCode::BadParams, "cue text too long", "This text is too long for one cue.");
  const std::string remember = strParam(p, "remember", "");
  if (!remember.empty() && remember != "phrase" && remember != "cue")
    fail(ErrorCode::BadParams, "remember must be phrase, cue or null", "The request was malformed.");
  vp::CueRecord& r = project_.cues[pos];
  // "Add to my corrections" sends the text the cue already has: only the correction is added (no second history
  // step, no re-check).
  const bool onlyRemember = !remember.empty() && text == r.target && r.state == "edited";
  if (!onlyRemember) {
    HistoryStep step;
    step.label = "cue.set";
    step.before.emplace_back(pos, r);
    retarget(pos, text);
    r.state = "edited";
    r.edited = true;
    r.reviewed = false;
    r.chosen = -1;
    step.after.emplace_back(pos, r);
    pushHistory(std::move(step));
  }
  json out{{"cue", cueView(pos)}};
  if (!remember.empty()) {
    const std::string key = sourceKey(langs_.source, sources_[pos]);
    vp::Correction* found = nullptr;
    for (vp::Correction& c : project_.corrections)
      if (c.key == key && c.scope == remember) found = &c;
    if (found) {
      found->target = text;
      ++found->count;
    } else {
      vp::Correction c;
      c.id = "c" + std::to_string(nextCorrection_++);
      c.key = key;
      c.target = text;
      c.scope = remember;
      c.count = 1;
      project_.corrections.push_back(c);
      found = &project_.corrections.back();
    }
    out["correctionAdded"] =
        json{{"id", found->id}, {"key", found->key}, {"target", found->target}, {"scope", found->scope}, {"count", found->count}};
  }
  noteChange();
  return out;
}

json Server::cmdCueChoose(const json& p) {
  const size_t pos = position(p);
  vp::CueRecord& r = project_.cues[pos];
  const int64_t alt = intParam(p, "alternative", 0, 0, static_cast<int64_t>(r.alternatives.size()) - 1, true);
  HistoryStep step;
  step.label = "cue.choose";
  step.before.emplace_back(pos, r);
  const std::string text = r.alternatives[static_cast<size_t>(alt)].text;
  retarget(pos, text);
  r.chosen = static_cast<int>(alt);
  if (r.state == "new" || r.state == "stale") r.state = "translated";
  step.after.emplace_back(pos, r);
  pushHistory(std::move(step));
  noteChange();
  return json{{"cue", cueView(pos)}};
}

json Server::cmdCueReview(const json& p) {
  requireProject();
  const json* idx = param(p, "indices");
  if (!idx || !idx->is_array()) fail(ErrorCode::BadParams, "missing array 'indices'", "The request was incomplete.");
  const bool reviewed = boolParam(p, "reviewed", true);
  std::vector<size_t> positions;
  for (const json& v : *idx) {
    if (!v.is_number_integer() || v.get<int64_t>() < 0 || v.get<int64_t>() >= static_cast<int64_t>(project_.cues.size()))
      fail(ErrorCode::BadParams, "bad cue index in 'indices'", "A cue number was out of range.");
    positions.push_back(static_cast<size_t>(v.get<int64_t>()));
  }
  std::sort(positions.begin(), positions.end());
  positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
  HistoryStep step;
  step.label = "cue.review";
  int count = 0;
  for (size_t pos : positions) {
    vp::CueRecord& r = project_.cues[pos];
    if (r.reviewed == reviewed) continue;
    step.before.emplace_back(pos, r);
    r.reviewed = reviewed;
    r.state = reviewed ? "reviewed" : stateAfterUnreview(r);
    step.after.emplace_back(pos, r);
    ++count;
  }
  pushHistory(std::move(step));
  if (count) noteChange();
  return json{{"count", count}};
}

// ---------------------------------------------------------------------------------------------- jobs
json Server::cmdTranslateStart(const json& p) {
  requireProject();
  vp::rules::Options opt = engineOptions();
  opt.fidelity = static_cast<int>(intParam(p, "fidelity", opt.fidelity, 1, 3));
  if (const json* e = param(p, "engines")) {
    if (!e->is_object()) fail(ErrorCode::BadParams, "'engines' must be an object", "The request was malformed.");
    opt.useModel = boolParam(*e, "model", false);
    opt.useOnline = boolParam(*e, "online", false);
  }
  opt.orbergise = boolParam(p, "orbergise", false);
  requirePair(project_.manifest.pair);   // e.g. no english.*.vpt -> not_found with a hint, before any work
  if (opt.orbergise) requirePair("la-la");
  // A requested engine that cannot run is not dropped silently: the result lists the codes (`warnings`) and the
  // job starts with one `translate.warning {jobId, engine, code, message, hint}` event per engine.
  std::vector<std::pair<std::string, vp::Error>> warns;
  if (opt.useModel) {
    std::string why;
    if (!modelUsable(why)) {
      opt.useModel = false;
      const ErrorCode c = why == "model_unsupported_cpu" ? ErrorCode::ModelUnsupportedCpu
                          : why == "model_missing"       ? ErrorCode::ModelMissing
                                                         : ErrorCode::ModelLoadFailed;
      const char* hint = c == ErrorCode::ModelUnsupportedCpu ? "This computer's processor lacks the AVX2 instructions the local model needs."
                         : c == ErrorCode::ModelMissing      ? "The local model is not installed. The rule engine translates on its own."
                                                             : "The local model file is not the expected one. The rule engine translates on its own.";
      warns.emplace_back("model", vp::Error{c, "local model not usable: " + why, hint});
    }
  }
  if (opt.useOnline && !vp::online::onlineAllowed(settings_)) {   // engines.online && online.wiktionary
    opt.useOnline = false;
    warns.emplace_back("online", vp::Error{ErrorCode::OnlineDisabled, "online check is turned off in the settings",
                                           "Turn on the online check in Settings to use it."});
  }
  bool given = false;
  std::vector<size_t> positions = indicesParam(p, &given);
  if (!given)
    for (size_t i = 0; i < project_.cues.size(); ++i)
      if (!project_.cues[i].edited && !project_.cues[i].reviewed) positions.push_back(i);
  const int64_t jobId = nextJob_++;
  json codes = json::array();
  for (const auto& w : warns) codes.push_back(vp::errorCodeName(w.second.code));
  pendingJob_ = [this, jobId, positions, opt, warns]() {
    // Engine ii: the model is loaded lazily on the engine's first question inside this job and unloaded when the
    // job ends, whatever happens.
    struct UnloadAfter {
      vp::llm::Model& m;
      ~UnloadAfter() { m.unload(); }
    } guard{model_};
    runJob(jobId, positions, opt, "translate", warns);
  };
  return json{{"jobId", jobId}, {"total", positions.size()}, {"warnings", codes}};
}

json Server::cmdOrbergiseStart(const json& p) {
  requireProject();
  vp::rules::Options opt = engineOptions();
  opt.orbergise = true;
  opt.source = vp::rules::Lang::La;
  opt.target = vp::rules::Lang::La;
  opt.orbergTier = static_cast<int>(intParam(p, "tier", 1, 1, 2));
  boolParam(p, "keepNames", true);   // the rules engine has no switch for these yet (engine/cli/README.md)
  boolParam(p, "simplify", false);
  const std::string original = strParam(p, "originalPath", "");
  if (langs_.source != vp::rules::Lang::La)
    fail(ErrorCode::BadParams, "Orbergise needs a Latin project (pair la-la)", "Open a Latin file to orbergise it.");
  requirePair("la-la");
  if (!original.empty()) loadOriginal(original);
  bool given = false;
  std::vector<size_t> positions = indicesParam(p, &given);
  if (!given)
    for (size_t i = 0; i < project_.cues.size(); ++i)
      if (!project_.cues[i].edited && !project_.cues[i].reviewed) positions.push_back(i);
  const int64_t jobId = nextJob_++;
  pendingJob_ = [this, jobId, positions, opt]() { runJob(jobId, positions, opt, "translate", {}); };
  return json{{"jobId", jobId}, {"total", positions.size()}, {"warnings", json::array()}};
}

void Server::runJob(int64_t jobId, std::vector<size_t> positions, vp::rules::Options opt, const char* prefix,
                    std::vector<std::pair<std::string, vp::Error>> warnings) {
  const std::string ev = prefix;
  runningJob_.store(jobId);
  jobId_ = jobId;
  for (const auto& w : warnings) jobWarning(w.first.c_str(), w.second);
  // advisors for this job only (server_engine.cpp); the engine calls them on this thread
  jobModel_ = opt.useModel ? vp::llm::makeAdvisors(model_, modelConfig()) : vp::llm::Advisors{};
  jobModelFailed_ = false;
  jobOnline_ = opt.useOnline;
  jobOnlineFailed_ = false;
  jobTarget_ = opt.target;
  struct ClearJob {
    Server& s;
    ~ClearJob() {
      s.jobModel_ = vp::llm::Advisors{};
      s.jobOnline_ = false;
      s.batchFacts_ = JobFacts{};
    }
  } clearJob{*this};
  const int64_t t0 = monoMs();
  int64_t lastProgress = 0;
  const size_t total = positions.size();
  size_t done = 0;
  bool failed = false;
  auto cancelled = [this, jobId]() { return cancelJob_.load() == jobId || stop_.load() || stopRequested(); };
  auto progressEvent = [&](size_t d, bool force) {
    const int64_t now = monoMs();
    if (!force && now - lastProgress < kProgressMs) return;
    lastProgress = now;
    const double secs = std::max(0.001, static_cast<double>(now - t0) / 1000.0);
    const double rate = static_cast<double>(d) / secs;
    const double eta = rate > 0 ? static_cast<double>(total - d) / rate : 0.0;
    sendEvent((ev + ".progress").c_str(), json{{"jobId", jobId}, {"done", d}, {"total", total},
                                               {"cuesPerSec", std::round(rate * 10.0) / 10.0},
                                               {"etaSec", std::round(eta * 10.0) / 10.0}});
  };
  const vp::rules::Context ctx = engineContext();
  HistoryStep step;
  step.label = ev;
  int conf[3] = {0, 0, 0};
  progressEvent(0, true);
  for (size_t at = 0; at < total && !cancelled(); at += kBatch) {
    const size_t n = std::min(kBatch, total - at);
    std::vector<vp::rules::CueInput> inputs;
    inputs.reserve(n);
    for (size_t k = 0; k < n; ++k) inputs.push_back(cueInput(positions[at + k]));
    const size_t base = done;
    batchFacts_ = JobFacts{};
    batchFacts_.model = opt.useModel;
    batchFacts_.online = opt.useOnline;
    batchFacts_.orberg = opt.orbergise;
    vp::Result<std::vector<vp::rules::CueOutput>> r =
        engine_->translate(inputs, opt, ctx, [&](size_t k) { progressEvent(base + k, false); }, cancelled);
    if (!r) {
      sendEvent((ev + ".error").c_str(), json{{"jobId", jobId}, {"code", vp::errorCodeName(r.error().code)},
                                              {"message", r.error().message}, {"hint", r.error().hint}});
      failed = true;
      break;
    }
    json cues = json::array();
    const size_t got = std::min(n, r->size());
    for (size_t k = 0; k < got; ++k) {
      const size_t pos = positions[at + k];
      vp::CueRecord& rec = project_.cues[pos];
      step.before.emplace_back(pos, rec);
      storeOutput(pos, r.value()[k]);
      rec.state = "translated";
      rec.edited = false;
      rec.reviewed = false;
      step.after.emplace_back(pos, rec);
      if (rec.confidence == "ok") ++conf[0];
      else if (rec.confidence == "fix") ++conf[2];
      else ++conf[1];
      cues.push_back(cueView(pos));
    }
    done += got;
    if (got) {
      noteChange();
      sendEvent((ev + ".cue").c_str(), json{{"jobId", jobId}, {"cues", std::move(cues)}});
    }
    progressEvent(done, false);
    tick(false);
    if (got < n) break;   // the engine stopped early (cancelled)
  }
  const bool wasCancelled = !failed && done < total;
  pushHistory(std::move(step));
  progressEvent(done, true);
  const int64_t ms = monoMs() - t0;
  if (!failed)
    sendEvent((ev + ".done").c_str(),
              json{{"jobId", jobId},
                   {"stats", {{"done", done}, {"translated", done}, {"total", total}, {"cancelled", wasCancelled}, {"ok", conf[0]},
                              {"check", conf[1]}, {"fix", conf[2]}, {"ms", ms},
                              {"cuesPerSec", ms > 0 ? std::round(static_cast<double>(done) * 10000.0 / ms) / 10.0 : 0.0}}}});
  logMsg(LogLevel::Info, ev + " job " + std::to_string(jobId) + ": " + std::to_string(done) + "/" +
                             std::to_string(total) + " cues in " + std::to_string(ms) + " ms" +
                             (wasCancelled ? " (cancelled)" : ""));
  runningJob_.store(0);
}

// ---------------------------------------------------------------------------------------------- history
json Server::historyJson(const std::vector<size_t>& changed) const {
  json idx = json::array();
  for (size_t pos : changed) idx.push_back(pos);
  return json{{"canUndo", !undo_.empty()}, {"canRedo", !redo_.empty()}, {"changedIndices", idx}};
}

json Server::cmdHistoryUndo(const json&) {
  requireProject();
  std::vector<size_t> changed;
  if (!undo_.empty()) {
    HistoryStep s = std::move(undo_.back());
    undo_.pop_back();
    for (auto it = s.before.rbegin(); it != s.before.rend(); ++it) {
      if (it->first < project_.cues.size()) project_.cues[it->first] = it->second;
      changed.push_back(it->first);
    }
    redo_.push_back(std::move(s));
    noteChange();
  }
  std::sort(changed.begin(), changed.end());
  changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
  return historyJson(changed);
}

json Server::cmdHistoryRedo(const json&) {
  requireProject();
  std::vector<size_t> changed;
  if (!redo_.empty()) {
    HistoryStep s = std::move(redo_.back());
    redo_.pop_back();
    for (const auto& a : s.after) {
      if (a.first < project_.cues.size()) project_.cues[a.first] = a.second;
      changed.push_back(a.first);
    }
    undo_.push_back(std::move(s));
    noteChange();
  }
  std::sort(changed.begin(), changed.end());
  changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
  return historyJson(changed);
}

// ---------------------------------------------------------------------------------------------- names, corrections
json Server::cmdNamesList(const json&) {
  requireProject();
  json names = json::array();
  for (const vp::NameEntry& n : project_.glossary)
    names.push_back(json{{"name", n.name}, {"policy", n.policy}, {"form", n.form}, {"forms", n.forms},
                         {"gender", n.gender}, {"declension", n.declension}});
  return json{{"names", names}};
}

json Server::cmdNamesSet(const json& p) {
  requireProject();
  const std::string name = strParam(p, "name", "", true);
  if (name.empty() || name.size() > 200) fail(ErrorCode::BadParams, "bad name", "Enter a name.");
  const std::string policy = strParam(p, "policy", "keep");
  if (policy != "keep" && policy != "decline" && policy != "translate")
    fail(ErrorCode::BadParams, "policy must be keep, decline or translate", "Choose what to do with the name.");
  std::string declension;
  if (const json* d = param(p, "declension")) {
    if (d->is_number_integer()) declension = std::to_string(d->get<int64_t>());
    else if (d->is_string()) declension = d->get<std::string>();
    else fail(ErrorCode::BadParams, "declension must be a number or a string", "The request was malformed.");
  }
  vp::NameEntry* e = nullptr;
  for (vp::NameEntry& n : project_.glossary)
    if (n.name == name) e = &n;
  if (!e) {
    project_.glossary.emplace_back();
    e = &project_.glossary.back();
    e->name = name;
  }
  e->policy = policy;
  e->form = strParam(p, "form", e->form);
  e->gender = strParam(p, "gender", e->gender);
  if (param(p, "declension")) e->declension = declension;
  json affected = json::array();
  for (size_t i = 0; i < sources_.size(); ++i) {
    if (!containsWord(sources_[i], name)) continue;
    affected.push_back(i);
    if (project_.cues[i].state == "translated") project_.cues[i].state = "stale";
  }
  noteChange();
  return json{{"affectedCues", affected}};
}

json Server::cmdCorrectionsList(const json&) {
  requireProject();
  json list = json::array();
  for (const vp::Correction& c : project_.corrections)
    list.push_back(json{{"id", c.id}, {"key", c.key}, {"target", c.target}, {"scope", c.scope}, {"count", c.count}});
  return json{{"corrections", list}};
}

json Server::cmdCorrectionsRemove(const json& p) {
  requireProject();
  const std::string id = strParam(p, "id", "", true);
  auto it = std::find_if(project_.corrections.begin(), project_.corrections.end(),
                         [&](const vp::Correction& c) { return c.id == id; });
  if (it == project_.corrections.end()) fail(ErrorCode::NotFound, "no correction '" + id + "'", "That correction no longer exists.");
  project_.corrections.erase(it);
  noteChange();
  return cmdCorrectionsList(p);
}

// ---------------------------------------------------------------------------------------------- words, eval
json Server::cmdWordsList(const json& p) {
  requireProject();
  const vp::lex::Lexicon* lx = lexFor(langs_.target);
  if (!lx)
    fail(ErrorCode::LexiconMissing, std::string("no lexicon for '") + langCode(langs_.target) + "'",
         "The dictionary for the target language is not installed.");
  const int64_t limit = intParam(p, "limit", 2000, 1, 100000);
  const bool greek = langs_.target == vp::rules::Lang::Grc;
  const bool classical = greek || langs_.target == vp::rules::Lang::La;
  std::map<uint32_t, std::pair<int, int>> counts;   // lemma -> (count, effective tier of its first token)
  int known = 0, unknown = 0, names = 0;
  std::vector<vp::lex::Analysis> an;
  std::vector<vp::rules::TokenView> toks;
  std::string word;
  auto count = [&](uint32_t id, int tier) {
    auto it = counts.find(id);
    if (it == counts.end()) counts.emplace(id, std::make_pair(1, tier));
    else ++it->second.first;
    ++known;
  };
  // cues without stored tokens (edited before C8, other engines): the words of the target looked up
  auto flush = [&]() {
    if (word.empty()) return;
    const std::string key = classical ? lexKey(word, greek) : vp::text::lower(word);
    word.clear();
    if (key.empty()) return;
    an.clear();
    if (lx->lookup(key, an) && !an.empty()) count(an[0].lemma, lx->lemma(an[0].lemma).tier);
    else ++unknown;
  };
  for (const vp::CueRecord& r : project_.cues) {
    if (r.target.empty()) continue;
    const vp::CueReason* h = hiddenReason(r, kTokensKind);
    if (h && decodeTokens(h->data, toks)) {   // the engine's own tokens: lemma ids and effective tiers
      for (const vp::rules::TokenView& t : toks) {
        if (t.hasLemma && lx->lemma(t.lemmaId).id != vp::lex::kNoLemma) {
          count(t.lemmaId, t.tier ? t.tier : lx->lemma(t.lemmaId).tier);
        } else if (!t.unknown && !t.text.empty() && t.text[0] != '[' && (t.text[0] & 0x80 || (t.text[0] >= 'A' && t.text[0] <= 'Z'))) {
          ++names;   // a capitalised word kept as a name
          ++known;
        } else if (!t.text.empty()) {
          ++unknown;
        }
      }
      continue;
    }
    for (size_t i = 0; i < r.target.size();) {
      const size_t at = i;
      const char32_t cp = vp::text::decodeUtf8(r.target, i);
      const bool sep = cp < 0x80 && !((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '-');
      if (sep || cp == 0x2014 || cp == 0x2013 || cp == 0x00AB || cp == 0x00BB || cp == 0x2019 || cp == 0x201C ||
          cp == 0x201D || cp == 0x00B7 || cp == 0x0387)
        flush();
      else
        word.append(r.target, at, i - at);
    }
    flush();
  }
  std::vector<std::pair<uint32_t, std::pair<int, int>>> sorted(counts.begin(), counts.end());
  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
    return a.second.first != b.second.first ? a.second.first > b.second.first : a.first < b.first;
  });
  int tiers[4] = {0, 0, 0, names};   // t1 t2 t3 names
  json words = json::array();
  for (const auto& kv : sorted) {
    const vp::lex::Lemma l = lx->lemma(kv.first);
    const int tier = kv.second.second;
    if (l.flags & vp::lex::ProperName) tiers[3] += kv.second.first;
    else if (tier >= 1 && tier <= 3) tiers[tier - 1] += kv.second.first;
    if (static_cast<int64_t>(words.size()) < limit) {
      json lemma = lemmaJson(*lx, kv.first);
      lemma["tier"] = tier;   // the tier the engine used (curated tiers override the lexicon's)
      words.push_back(json{{"lemma", lemma}, {"count", kv.second.first}, {"tier", tier}});
    }
  }
  auto share = [known](int n) { return known ? std::round(1000.0 * n / known) / 1000.0 : 0.0; };
  return json{{"words", words},
              {"tierShare", {{"t1", share(tiers[0])}, {"t2", share(tiers[1])}, {"t3", share(tiers[2])}, {"names", share(tiers[3])}}},
              {"tokens", known + unknown},
              {"unknown", unknown}};
}

json Server::cmdEvalRun(const json& p) {
  requireProject();
  std::string outPath = strParam(p, "outPath", "", true);
  if (fs::isDirectory(outPath)) outPath = fs::join(outPath, "report.json");
  const vp::rules::Options opt = engineOptions();
  const vp::rules::Context ctx = engineContext();
  int conf[4] = {0, 0, 0, 0};   // ok check fix untranslated
  std::map<std::string, std::pair<int, int>> checks;   // id -> (ok, fail)
  json perCue = json::array();
  for (size_t i = 0; i < project_.cues.size(); ++i) {
    const vp::CueRecord& r = project_.cues[i];
    if (r.target.empty()) {
      ++conf[3];
      continue;
    }
    vp::Result<vp::rules::CueOutput> out = engine_->check(cueInput(i), r.target, opt, ctx);
    if (!out) {
      ++conf[3];
      continue;
    }
    const std::string c = confidenceName(out->confidence);
    ++conf[c == "ok" ? 0 : c == "check" ? 1 : 2];
    json failed = json::array();
    for (const auto& ch : out->checks) {
      auto& slot = checks[ch.id];
      if (ch.ok) ++slot.first;
      else {
        ++slot.second;
        failed.push_back(ch.id);
      }
    }
    if (!failed.empty() || c != "ok") perCue.push_back(json{{"index", i}, {"confidence", c}, {"failed", failed}});
  }
  json checkJsonObj = json::object();
  for (const auto& kv : checks) checkJsonObj[kv.first] = json{{"ok", kv.second.first}, {"fail", kv.second.second}};
  json report{{"engine", engine_->version()},
              {"appVersion", vp::appVersion()},
              {"pair", project_.manifest.pair},
              {"source", project_.manifest.sourceFileName},
              {"cues", project_.cues.size()},
              {"confidence", {{"ok", conf[0]}, {"check", conf[1]}, {"fix", conf[2]}, {"untranslated", conf[3]}}},
              {"checks", checkJsonObj},
              {"perCue", perCue}};
  unwrap(fs::writeFileAtomic(outPath, report.dump(2) + "\n"));
  return json{{"reportPath", outPath}};
}

}  // namespace vpcli
