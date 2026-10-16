// TESTS ONLY (VP_ONLINE_MOCK): see online_mock.h.
#include "online_mock.h"

#include <string>

#include "json.hpp"
#include "vp/text.h"

namespace vpcli {

namespace {
int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string percentDecode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
      out.push_back(static_cast<char>(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2])));
      i += 2;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

class MockTransport final : public vp::online::Transport {
 public:
  MockTransport(const vp::lex::Lexicon* la, bool disagree, std::atomic<uint64_t>* calls)
      : la_(la), disagree_(disagree), calls_(calls) {}

  vp::Result<vp::online::Response> get(const std::string& url, int, const vp::online::Headers&) override {
    if (calls_) calls_->fetch_add(1);
    vp::online::Response r;
    const size_t slash = url.rfind('/');
    const std::string title = percentDecode(slash == std::string::npos ? url : url.substr(slash + 1));
    std::string gloss;
    if (disagree_) {
      gloss = "a kind of musical instrument played in winter";
    } else if (la_) {
      std::vector<vp::lex::Analysis> an;
      if (la_->lookup(vp::text::latin_key(title), an) && !an.empty()) gloss = std::string(la_->lemma(an[0].lemma).glossEn);
    }
    if (gloss.empty()) {
      r.status = 404;
      r.body = R"({"title":"Not found."})";
      return vp::Result<vp::online::Response>(std::move(r));
    }
    nlohmann::json body{{"la", nlohmann::json::array({nlohmann::json{
                                   {"partOfSpeech", "Noun"},
                                   {"language", "Latin"},
                                   {"definitions", nlohmann::json::array({nlohmann::json{{"definition", gloss}}})}}})}};
    r.status = 200;
    r.body = body.dump();
    r.headers["content-type"] = "application/json";
    return vp::Result<vp::online::Response>(std::move(r));
  }

 private:
  const vp::lex::Lexicon* la_;
  bool disagree_;
  std::atomic<uint64_t>* calls_;
};
}  // namespace

std::unique_ptr<vp::online::Transport> makeMockTransport(const vp::lex::Lexicon* latin, bool disagree,
                                                         std::atomic<uint64_t>* calls) {
  return std::make_unique<MockTransport>(latin, disagree, calls);
}

}  // namespace vpcli
