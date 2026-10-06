// Cue commands, translate/orbergise jobs, history, names, corrections, words and eval.
#include <algorithm>
#include <cmath>
#include <map>

#include "server.h"
#include "vp/fs.h"
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
  json alts = json::array(), tokens = json::array(), checks = json::array(), reasons = json::array();
  for (const vp::Alternative& a : r.alternatives) alts.push_back(altJson(a));
  for (const vp::CueCheck& c : r.checks) checks.push_back(checkJson(c));
  for (const vp::CueReason& x : r.reasons) reasons.push_back(reasonJson(x));
  if (!r.target.empty()) {
    vp::Result<vp::rules::CueOutput> out = engine_->check(cueInput(pos), r.target, engineOptions(), engineContext());
    if (out)
      for (const vp::rules::TokenView& t : out->tokens) tokens.push_back(tokenJson(t));
  }
  return json{{"cue", cueView(pos)}, {"alternatives", alts}, {"tokens", tokens}, {"checks", checks}, {"reasons", reasons}};
}

json Server::cmdCueSet(const json& p) {
  const size_t pos = position(p);
  const std::string text = strParam(p, "text", "", true);
  if (text.size() > 64 * 1024) fail(ErrorCode::BadParams, "cue text too long", "This text is too long for one cue.");
  const std::string remember = strParam(p, "remember", "");
  if (!remember.empty() && remember != "phrase" && remember != "cue")
    fail(ErrorCode::BadParams, "remember must be phrase, cue or null", "The request was malformed.");
  vp::CueRecord& r = project_.cues[pos];
  HistoryStep step;
  step.label = "cue.set";
  step.before.emplace_back(pos, r);
  r.target = text;
  r.state = "edited";
  r.edited = true;
  r.reviewed = false;
  r.chosen = -1;
  vp::Result<vp::rules::CueOutput> chk = engine_->check(cueInput(pos), text, engineOptions(), engineContext());
  if (chk) {
    r.confidence = confidenceName(chk->confidence);
    r.score = chk->score;
    r.checks.clear();
    for (const auto& c : chk->checks) r.checks.push_back(vp::CueCheck{c.id, c.ok, c.detail});
  } else {
    logMsg(LogLevel::Warn, "check after edit: " + chk.error().message);
  }
  step.after.emplace_back(pos, r);
  pushHistory(std::move(step));
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
  r.target = r.alternatives[static_cast<size_t>(alt)].text;
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
  json warnings = json::array();
  if (const json* e = param(p, "engines")) {
    if (!e->is_object()) fail(ErrorCode::BadParams, "'engines' must be an object", "The request was malformed.");
    opt.useModel = boolParam(*e, "model", false);
    opt.useOnline = boolParam(*e, "online", false);
  }
  if (opt.useModel) {
    opt.useModel = false;
    warnings.push_back("model_missing");
  }
  if (opt.useOnline) {
    const json s = settings_.get();
    const bool allowed = s.contains("engines") && s["engines"].value("online", false);
    if (!allowed) warnings.push_back("online_disabled");
    opt.useOnline = false;   // no online client in this build
  }
  opt.orbergise = boolParam(p, "orbergise", false);
  std::vector<size_t> positions;
  if (const json* idx = param(p, "indices")) {
    if (!idx->is_array()) fail(ErrorCode::BadParams, "'indices' must be an array", "The request was malformed.");
    for (const json& v : *idx) {
      if (!v.is_number_integer() || v.get<int64_t>() < 0 || v.get<int64_t>() >= static_cast<int64_t>(project_.cues.size()))
        fail(ErrorCode::BadParams, "bad cue index in 'indices'", "A cue number was out of range.");
      positions.push_back(static_cast<size_t>(v.get<int64_t>()));
    }
    std::sort(positions.begin(), positions.end());
    positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
  } else {
    for (size_t i = 0; i < project_.cues.size(); ++i)
      if (!project_.cues[i].edited && !project_.cues[i].reviewed) positions.push_back(i);
  }
  const int64_t jobId = nextJob_++;
  pendingJob_ = [this, jobId, positions, opt]() { runJob(jobId, positions, opt, "translate"); };
  return json{{"jobId", jobId}, {"total", positions.size()}, {"warnings", warnings}};
}

json Server::cmdOrbergiseStart(const json& p) {
  requireProject();
  vp::rules::Options opt = engineOptions();
  opt.orbergise = true;
  opt.orbergTier = static_cast<int>(intParam(p, "tier", 1, 1, 2));
  const bool keepNames = boolParam(p, "keepNames", true);
  boolParam(p, "simplify", false);
  strParam(p, "originalPath", "");
  (void)keepNames;
  std::vector<size_t> positions;
  for (size_t i = 0; i < project_.cues.size(); ++i)
    if (!project_.cues[i].edited && !project_.cues[i].reviewed) positions.push_back(i);
  const int64_t jobId = nextJob_++;
  pendingJob_ = [this, jobId, positions, opt]() { runJob(jobId, positions, opt, "translate"); };
  return json{{"jobId", jobId}, {"total", positions.size()}};
}

void Server::runJob(int64_t jobId, std::vector<size_t> positions, vp::rules::Options opt, const char* prefix) {
  const std::string ev = prefix;
  runningJob_.store(jobId);
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
      applyOutput(r.value()[k], rec);
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
  std::map<uint32_t, int> counts;
  int known = 0, unknown = 0;
  std::vector<vp::lex::Analysis> an;
  std::string word;
  auto flush = [&]() {
    if (word.empty()) return;
    const std::string key = classical ? lexKey(word, greek) : vp::text::lower(word);
    word.clear();
    if (key.empty()) return;
    an.clear();
    if (lx->lookup(key, an) && !an.empty()) {
      ++counts[an[0].lemma];
      ++known;
    } else {
      ++unknown;
    }
  };
  for (const vp::CueRecord& r : project_.cues) {
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
  std::vector<std::pair<uint32_t, int>> sorted(counts.begin(), counts.end());
  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
    return a.second != b.second ? a.second > b.second : a.first < b.first;
  });
  int tiers[4] = {0, 0, 0, 0};   // t1 t2 t3 names
  json words = json::array();
  for (const auto& kv : sorted) {
    const vp::lex::Lemma l = lx->lemma(kv.first);
    if (l.flags & vp::lex::ProperName) tiers[3] += kv.second;
    else if (l.tier >= 1 && l.tier <= 3) tiers[l.tier - 1] += kv.second;
    if (static_cast<int64_t>(words.size()) < limit)
      words.push_back(json{{"lemma", lemmaJson(*lx, kv.first)}, {"count", kv.second}, {"tier", l.tier}});
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
