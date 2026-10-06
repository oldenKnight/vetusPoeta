// Greedy arc-eager decoder; mirror of parse_sentence() in tools/train/train_parser.py.
#include <algorithm>

#include "feature_strings.h"
#include "model.h"
#include "vp/nlp.h"

namespace vp::nlp {
namespace {

enum { kShift = 0, kReduce = 1, kLeft = 2, kRight = 3, kTrans = 4 };
const char* const kTransNames[kTrans] = {"@SHIFT", "@REDUCE", "@LEFT", "@RIGHT"};

bool valid(const detail::ParseState& st, int t) {
  if (t == kShift) return st.b <= st.n;
  if (st.stack.empty()) return false;
  const int s0 = st.stack.back();
  if (t == kReduce) return s0 != 0 && st.heads[static_cast<size_t>(s0)] >= 0;
  if (t == kLeft) return s0 != 0 && st.heads[static_cast<size_t>(s0)] < 0 && st.b <= st.n;
  return st.b <= st.n && (s0 != 0 || st.nr[0] == 0);
}

}  // namespace

Parser::Parser() = default;
Parser::~Parser() = default;
Parser::Parser(Parser&&) noexcept = default;
Parser& Parser::operator=(Parser&&) noexcept = default;

Result<Parser> Parser::open(const std::string& path) {
  auto m = Model::open(path, "dep");
  if (!m.ok()) return m.error();
  const Model& mm = m.value();
  bool ok = mm.nLabels > kTrans && mm.label("root") >= kTrans;
  for (int k = 0; ok && k < kTrans; ++k) ok = mm.labels[static_cast<size_t>(k)] == kTransNames[k];
  if (!ok) return Error{ErrorCode::Internal, "model " + path + ": label layout is not a parser's", "model file damaged"};
  Parser p;
  p.model_ = std::make_unique<Model>(std::move(m.value()));
  return p;
}

std::string_view Parser::lang() const { return model_ ? model_->lang : std::string_view(); }
std::string_view Parser::note() const { return model_ ? model_->note : std::string_view(); }

void Parser::parse(std::vector<Token>& sentence) const {
  if (!model_) return;
  const Model& m = *model_;
  const int n = static_cast<int>(sentence.size());
  const int nlab = static_cast<int>(m.nLabels);
  const int rootIdx = m.label("root");
  std::vector<std::string_view> lows(static_cast<size_t>(n) + 1), tags(static_cast<size_t>(n) + 1);
  lows[0] = detail::kRoot;
  tags[0] = detail::kRoot;
  for (int i = 0; i < n; ++i) {
    lows[static_cast<size_t>(i) + 1] = sentence[static_cast<size_t>(i)].lower;
    tags[static_cast<size_t>(i) + 1] = sentence[static_cast<size_t>(i)].upos;
  }
  detail::ParseState st;
  st.reset(n);
  detail::FeatBuf fb;
  std::vector<int32_t> sc(m.nLabels), sc2(m.nLabels);
  while (st.b <= n) {
    fb.reset();
    detail::parseFeatures(lows, tags, st, fb);
    std::fill(sc.begin(), sc.end(), 0);
    for (size_t k = 0; k < fb.n; ++k) m.add(featureHash(fb.v[k]), sc.data());
    int t = -1;
    for (int k = 0; k < kTrans; ++k)
      if (valid(st, k) && (t < 0 || sc[static_cast<size_t>(k)] > sc[static_cast<size_t>(t)])) t = k;
    std::string_view label;
    int h = 0, d = 0;
    if (t == kLeft || t == kRight) {
      h = t == kLeft ? st.b : st.stack.back();
      d = t == kLeft ? st.stack.back() : st.b;
      if (h == 0) {
        label = "root";
      } else {
        fb.reset();
        detail::labelFeatures(lows, tags, h, d, t == kLeft ? "L" : "R", st, fb);
        sc2 = sc;
        for (size_t k = 0; k < fb.n; ++k) m.add(featureHash(fb.v[k]), sc2.data());
        int best = -1;
        for (int k = kTrans; k < nlab; ++k)
          if (k != rootIdx && (best < 0 || sc2[static_cast<size_t>(k)] > sc2[static_cast<size_t>(best)])) best = k;
        label = best >= 0 ? m.labels[static_cast<size_t>(best)] : std::string_view("dep");
      }
    }
    switch (t) {
      case kShift: st.stack.push_back(st.b++); break;
      case kReduce: st.stack.pop_back(); break;
      case kLeft: st.stack.pop_back(); st.arc(h, d, label); break;
      default: st.arc(h, d, label); st.stack.push_back(st.b++); break;
    }
  }
  // words left without a head (train_parser.State.finish)
  int root = st.rc1[0];
  for (int d = 1; d <= n; ++d) {
    if (st.heads[static_cast<size_t>(d)] >= 0) continue;
    if (root < 0) {
      st.arc(0, d, "root");
      root = d;
    } else {
      st.arc(root, d, tags[static_cast<size_t>(d)] == "PUNCT" ? "punct" : "dep");
    }
  }
  for (int i = 0; i < n; ++i) {
    sentence[static_cast<size_t>(i)].head = st.heads[static_cast<size_t>(i) + 1];
    sentence[static_cast<size_t>(i)].deprel.assign(st.labels[static_cast<size_t>(i) + 1]);
  }
}

}  // namespace vp::nlp
