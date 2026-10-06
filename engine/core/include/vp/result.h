// vp::Result<T> — the one error-carrying return type used across every module boundary. [CONTRACT, DESIGN.md §5.1, §9]
// No exception ever crosses a module boundary; functions that can fail return Result<T> (or Result<void>).
#pragma once
#include <string>
#include <utility>

namespace vp {

// Error codes are the protocol's error codes (DESIGN.md §9), spelled identically on the wire.
enum class ErrorCode {
  Ok = 0, BadParams, NotFound, Io, UnsupportedFormat, LexiconMissing, LexiconCorrupt, LexiconVersion,
  ProjectCorrupt, ModelMissing, ModelLoadFailed, ModelUnsupportedCpu, OnlineDisabled, OnlineFailed, Busy, Internal
};

const char* errorCodeName(ErrorCode c);   // "bad_params", "not_found", ... (implemented in engine/core/src/result.cpp)

struct Error {
  ErrorCode code = ErrorCode::Ok;
  std::string message;   // for logs and developers (English)
  std::string hint;      // for the user, English; the UI translates by code and falls back to this text
  bool ok() const { return code == ErrorCode::Ok; }
};

template <class T> class Result {
 public:
  Result(T value) : value_(std::move(value)) {}                                  // success
  Result(Error e) : error_(std::move(e)) {}                                      // failure
  Result(ErrorCode c, std::string message, std::string hint = {}) : error_{c, std::move(message), std::move(hint)} {}
  bool ok() const { return error_.ok(); }
  explicit operator bool() const { return ok(); }
  const Error& error() const { return error_; }
  T& value() { return value_; }               // only valid when ok()
  const T& value() const { return value_; }
  T* operator->() { return &value_; }
  const T* operator->() const { return &value_; }
 private:
  T value_{};
  Error error_;
};

template <> class Result<void> {
 public:
  Result() = default;
  Result(Error e) : error_(std::move(e)) {}
  Result(ErrorCode c, std::string message, std::string hint = {}) : error_{c, std::move(message), std::move(hint)} {}
  bool ok() const { return error_.ok(); }
  explicit operator bool() const { return ok(); }
  const Error& error() const { return error_; }
 private:
  Error error_;
};

}  // namespace vp
