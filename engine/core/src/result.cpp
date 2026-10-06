// Wire names of the protocol error codes (DESIGN.md section 9). See vp/result.h.
#include "vp/result.h"

namespace vp {

const char* errorCodeName(ErrorCode c) {
  switch (c) {
    case ErrorCode::Ok: return "ok";
    case ErrorCode::BadParams: return "bad_params";
    case ErrorCode::NotFound: return "not_found";
    case ErrorCode::Io: return "io";
    case ErrorCode::UnsupportedFormat: return "unsupported_format";
    case ErrorCode::LexiconMissing: return "lexicon_missing";
    case ErrorCode::LexiconCorrupt: return "lexicon_corrupt";
    case ErrorCode::LexiconVersion: return "lexicon_version";
    case ErrorCode::ProjectCorrupt: return "project_corrupt";
    case ErrorCode::ModelMissing: return "model_missing";
    case ErrorCode::ModelLoadFailed: return "model_load_failed";
    case ErrorCode::ModelUnsupportedCpu: return "model_unsupported_cpu";
    case ErrorCode::OnlineDisabled: return "online_disabled";
    case ErrorCode::OnlineFailed: return "online_failed";
    case ErrorCode::Busy: return "busy";
    case ErrorCode::Internal: return "internal";
  }
  return "internal";
}

}  // namespace vp
