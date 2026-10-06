// Export, word/lemma lookups, model and online status.
#include <algorithm>

#include "server.h"
#include "vp/fs.h"
#include "vp/text.h"

namespace vpcli {

namespace fs = vp::fs;
using vp::ErrorCode;

ExportOptions Server::exportOptions(const json& p) const {
  const json s = settings_.get();
  const json ex = s.contains("export") && s["export"].is_object() ? s["export"] : json::object();
  ExportOptions o;
  o.format = format_;
  const std::string f = strParam(p, "format", "");
  if (!f.empty() && !formatFromName(f, o.format))
    fail(ErrorCode::UnsupportedFormat, "unknown export format '" + f + "'", "Choose SRT, VTT, ASS or TXT.");
  o.emoji = boolParam(p, "emoji", ex.value("emoji", false));
  o.macrons = boolParam(p, "macrons", ex.value("macrons", false));
  o.rebreak = boolParam(p, "rebreak", ex.value("rebreak", true));
  const std::string greek = strParam(p, "greek", "polytonic");
  if (greek != "polytonic" && greek != "monotonic")
    fail(ErrorCode::BadParams, "greek must be polytonic or monotonic", "The request was malformed.");
  o.monotonic = greek == "monotonic" && langs_.target == vp::rules::Lang::Grc;
  o.encoding = strParam(p, "encoding", "");
  if (const json* b = param(p, "bom")) {
    if (!b->is_boolean()) fail(ErrorCode::BadParams, "bom must be true or false", "The request was malformed.");
    o.bom = b->get<bool>();
  }
  o.cpsLimit = cpsLimit_;
  return o;
}

json Server::cmdExportWrite(const json& p) {
  requireProject();
  const std::string path = strParam(p, "path", "", true);
  if (path.empty()) fail(ErrorCode::BadParams, "empty path", "Choose where to save the file.");
  const bool overwrite = boolParam(p, "overwrite", false);
  const ExportOptions o = exportOptions(p);
  if (!overwrite && fs::fileExists(path)) {
    vp::Error e{ErrorCode::Io, "file exists: " + path, "The file already exists. Choose another name or allow overwriting."};
    throw CmdError{e};
  }
  std::vector<std::string> targets(project_.cues.size());
  for (size_t i = 0; i < project_.cues.size(); ++i) targets[i] = project_.cues[i].target;
  ExportResult r = unwrap(buildExport(doc_, targets, sources_, o));
  unwrap(fs::writeFileAtomic(path, std::string_view(reinterpret_cast<const char*>(r.bytes.data()), r.bytes.size())));
  json warns = json::array();
  for (const ExportWarning& w : r.warnings)
    warns.push_back(json{{"index", w.index >= 0 ? json(w.index) : json(nullptr)}, {"kind", w.kind}});
  return json{{"path", path}, {"bytes", r.bytes.size()}, {"warnings", warns}};
}

json Server::cmdExportPreview(const json& p) {
  requireProject();
  const ExportOptions o = exportOptions(p);
  const json* idx = param(p, "indices");
  if (!idx || !idx->is_array()) fail(ErrorCode::BadParams, "missing array 'indices'", "The request was incomplete.");
  if (idx->size() > 200) fail(ErrorCode::BadParams, "at most 200 indices", "Preview fewer cues at once.");
  json cues = json::array();
  for (const json& v : *idx) {
    if (!v.is_number_integer() || v.get<int64_t>() < 0 || v.get<int64_t>() >= static_cast<int64_t>(project_.cues.size()))
      fail(ErrorCode::BadParams, "bad cue index in 'indices'", "A cue number was out of range.");
    const size_t pos = static_cast<size_t>(v.get<int64_t>());
    cues.push_back(json{{"index", pos},
                        {"lines", previewLines(doc_.cues[pos], project_.cues[pos].target, sources_[pos], o)}});
  }
  return json{{"cues", cues}};
}

// ---------------------------------------------------------------------------------------------- words
json Server::cmdWordInspect(const json& p) {
  const std::string text = strParam(p, "text", "", true);
  if (text.empty() || text.size() > 256) fail(ErrorCode::BadParams, "text must be 1..256 bytes", "Select one word.");
  const std::string code = strParam(p, "lang", "la");
  vp::rules::Lang lang;
  if (!langFromCode(code, lang)) fail(ErrorCode::BadParams, "unknown language '" + code + "'", "The request was malformed.");
  vp::rules::Options opt;
  if (hasProject_) opt = engineOptions();
  vp::rules::InspectResult r = unwrap(engine_->inspect(text, lang, opt));
  const vp::lex::Lexicon* lx = lexFor(lang);
  json analyses = json::array();
  for (const vp::rules::Analysis& a : r.analyses) {
    json lemma = lx ? lemmaJson(*lx, a.lemmaId) : json(nullptr);
    if (lemma.is_null())
      lemma = json{{"id", a.lemmaId}, {"head", a.head}, {"glossEn", a.glossEn}, {"glossEs", a.glossEs}, {"tier", a.tier}};
    analyses.push_back(json{{"lemma", lemma}, {"features", featureJson(a.features)}, {"display", a.display}});
  }
  return json{{"analyses", analyses}, {"suggestions", r.suggestions}};
}

json Server::cmdLemmaGet(const json& p) {
  const std::string code = strParam(p, "lang", "la");
  vp::rules::Lang lang;
  if (!langFromCode(code, lang)) fail(ErrorCode::BadParams, "unknown language '" + code + "'", "The request was malformed.");
  const vp::lex::Lexicon* lx = lexFor(lang);
  if (!lx) fail(ErrorCode::LexiconMissing, "no lexicon for '" + code + "'", "The dictionary for this language is not installed.");
  const int64_t id = intParam(p, "id", 0, 0, 0xFFFFFFFEll, true);
  json lemma = lemmaJson(*lx, static_cast<uint32_t>(id));
  if (lemma.is_null()) fail(ErrorCode::NotFound, "no lemma " + std::to_string(id), "That word is not in the dictionary.");
  std::vector<vp::lex::Sense> senses;
  lx->senses(static_cast<uint32_t>(id), senses);
  json sj = json::array();
  for (const vp::lex::Sense& s : senses) sj.push_back(senseJson(s));
  std::vector<std::pair<uint32_t, std::string_view>> cells;
  lx->cells(static_cast<uint32_t>(id), cells);
  json cj = json::array();
  for (const auto& c : cells) {
    json cell{{"features", featureJson(featuresFromPacked(c.first))}, {"form", std::string(c.second)}};
    const std::vector<std::string> extra = extraNames(vp::feat::unpack(c.first).extra);
    if (!extra.empty()) cell["extra"] = extra;
    cj.push_back(std::move(cell));
  }
  return json{{"lemma", lemma}, {"senses", sj}, {"cells", cj}};
}

// ---------------------------------------------------------------------------------------------- model, online
json Server::cmdModelStatus(const json&) {
  const json s = settings_.get();
  return json{{"available", false}, {"path", s.value("modelPath", "")}, {"sizeBytes", 0}, {"sha256ok", false},
              {"loaded", false}, {"cpuOk", false}, {"lastLoadMs", 0}, {"reason", "not_built"}};
}

json Server::cmdModelLocate(const json& p) {
  const std::string path = strParam(p, "path", "", true);
  if (!fs::isRegularFile(path)) fail(ErrorCode::NotFound, "no file at " + path, "The model file was not found.");
  fail(ErrorCode::ModelMissing, "the local model engine is not part of this build",
       "The local model is not available in this version yet.");
}

json Server::cmdModelTest(const json&) {
  fail(ErrorCode::ModelMissing, "the local model engine is not part of this build",
       "The local model is not available in this version yet.");
}

json Server::cmdOnlineTest(const json&) {
  const json s = settings_.get();
  const bool enabled = s.contains("engines") && s["engines"].is_object() && s["engines"].value("online", false);
  if (!enabled)
    fail(ErrorCode::OnlineDisabled, "online check is turned off", "Turn on the online check in Settings first.");
  fail(ErrorCode::OnlineFailed, "this build has no online client", "The online check is not available in this version yet.");
}

}  // namespace vpcli
