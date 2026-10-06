// Test doubles for engine/online: a scripted transport that counts calls and a manual clock. Header-only, used by
// engine/tests/test_online.cpp (and by any test that must prove "zero network calls"). Never used in the product.
#pragma once
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "vp/online.h"

namespace vp::online {

// A clock that only moves when told to (sleepMs advances it and records the sleep).
class FakeClock : public Clock {
 public:
  int64_t mono = 1000000;
  int64_t unix = 1790000000000LL;   // 2026-09-21
  std::vector<int64_t> sleeps;
  int64_t monoMs() override { return mono; }
  int64_t unixMs() override { return unix; }
  void sleepMs(int64_t ms) override {
    sleeps.push_back(ms);
    advance(ms);
  }
  void advance(int64_t ms) {
    mono += ms;
    unix += ms;
  }
  int64_t slept() const {
    int64_t t = 0;
    for (int64_t s : sleeps) t += s;
    return t;
  }
};

// Scripted responses, in order; when the script is empty the default step answers. delayMs simulates latency on
// the given clock; a delay longer than the request timeout yields the "timed out" network error.
class MockTransport : public Transport {
 public:
  struct Step {
    int status = 200;
    std::string body;
    Headers headers;
    int delayMs = 0;
    bool fail = false;          // network error instead of a response
    std::string error = "connection refused";
  };
  explicit MockTransport(FakeClock* clock = nullptr) : clock_(clock) {}
  void push(Step s) { script_.push_back(std::move(s)); }
  void setDefault(Step s) { default_ = std::move(s); }
  int calls() const { return calls_; }
  const std::vector<std::string>& urls() const { return urls_; }
  const Headers& lastHeaders() const { return lastHeaders_; }
  std::vector<int64_t> callTimes;   // clock->mono at each call (when a clock is set)

  Result<Response> get(const std::string& url, int timeoutMs, const Headers& requestHeaders) override {
    ++calls_;
    urls_.push_back(url);
    lastHeaders_ = requestHeaders;
    if (clock_) callTimes.push_back(clock_->mono);
    Step s = default_;
    if (!script_.empty()) {
      s = std::move(script_.front());
      script_.pop_front();
    }
    if (clock_ && s.delayMs > 0) {
      if (s.delayMs > timeoutMs) {
        clock_->advance(timeoutMs);
        return Result<Response>(ErrorCode::OnlineFailed, "timed out after " + std::to_string(timeoutMs) + " ms");
      }
      clock_->advance(s.delayMs);
    }
    if (s.fail) return Result<Response>(ErrorCode::OnlineFailed, s.error);
    Response r;
    r.status = s.status;
    r.body = std::move(s.body);
    r.headers = std::move(s.headers);
    return r;
  }

 private:
  FakeClock* clock_;
  std::deque<Step> script_;
  Step default_{404, "{\"type\":\"not_found\",\"title\":\"Not found.\"}", {}, 0, false, {}};
  int calls_ = 0;
  std::vector<std::string> urls_;
  Headers lastHeaders_;
};

}  // namespace vp::online
