// Greedy tagger decoder; mirror of tag_sentence() in tools/train/train_tagger.py.
#include <algorithm>

#include "feature_strings.h"
#include "model.h"
#include "vp/nlp.h"

namespace vp::nlp {
namespace {

struct Layout {
  int nUpos = 0;
  std::vector<std::pair<int, int>> groups;   // [lo, hi) label ranges, each starting with "<Group>=_"
};

// UPOS labels first (no '='), then runs "<Group>=_", "<Group>=<Value>"...; false if the labels do not fit.
bool layoutOf(const Model& m, Layout& out) {
  const int n = static_cast<int>(m.labels.size());
  int i = 0;
  while (i < n && m.labels[static_cast<size_t>(i)].find('=') == std::string_view::npos) ++i;
  out.nUpos = i;
  if (i == 0) return false;
  while (i < n) {
    const std::string_view l = m.labels[static_cast<size_t>(i)];
    if (l.size() < 3 || l.substr(l.size() - 2) != "=_") return false;
    const std::string_view group = l.substr(0, l.size() - 1);   // "Number="
    int j = i + 1;
    while (j < n && m.labels[static_cast<size_t>(j)].substr(0, group.size()) == group) ++j;
    out.groups.emplace_back(i, j);
    i = j;
  }
  return true;
}

int argmax(const int32_t* s, int lo, int hi) {
  int best = lo;
  for (int k = lo + 1; k < hi; ++k)
    if (s[k] > s[best]) best = k;
  return best;
}

}  // namespace

Tagger::Tagger() = default;
Tagger::~Tagger() = default;
Tagger::Tagger(Tagger&&) noexcept = default;
Tagger& Tagger::operator=(Tagger&&) noexcept = default;

Result<Tagger> Tagger::open(const std::string& path) {
  auto m = Model::open(path, "tag");
  if (!m.ok()) return m.error();
  Layout layout;
  if (!layoutOf(m.value(), layout))
    return Error{ErrorCode::Internal, "model " + path + ": label layout is not a tagger's", "model file damaged"};
  Tagger t;
  t.model_ = std::make_unique<Model>(std::move(m.value()));
  return t;
}

std::string_view Tagger::lang() const { return model_ ? model_->lang : std::string_view(); }
std::string_view Tagger::note() const { return model_ ? model_->note : std::string_view(); }

void Tagger::tag(std::vector<Token>& sentence) const {
  if (!model_) return;
  const Model& m = *model_;
  Layout layout;
  layoutOf(m, layout);
  const size_t n = sentence.size();
  std::vector<std::string_view> words(n), lows(n);
  std::vector<std::string> shapes(n);
  for (size_t i = 0; i < n; ++i) {
    sentence[i].lower = normalise(sentence[i].text);
    words[i] = sentence[i].text;
    lows[i] = sentence[i].lower;
    detail::shape(sentence[i].text, shapes[i]);
  }
  std::vector<int32_t> sc(m.nLabels), ex(m.nLabels);
  detail::FeatBuf fb;
  std::string_view t1 = detail::kBos, t2 = detail::kBos2;
  for (size_t i = 0; i < n; ++i) {
    fb.reset();
    detail::tagFeatures(words, lows, shapes, i, t1, t2, fb);
    std::fill(sc.begin(), sc.end(), 0);
    for (size_t k = 0; k < fb.n; ++k) m.add(featureHash(fb.v[k]), sc.data());
    const int g = argmax(sc.data(), 0, layout.nUpos);
    const std::string_view tag = m.labels[static_cast<size_t>(g)];
    fb.reset();
    detail::tagFeatExtra(tag, lows[i], fb);
    std::fill(ex.begin(), ex.end(), 0);
    for (size_t k = 0; k < fb.n; ++k) m.add(featureHash(fb.v[k]), ex.data());
    for (size_t k = 0; k < ex.size(); ++k) ex[k] += sc[k];
    uint32_t feats = 0;
    for (const auto& grp : layout.groups) {
      const int k = argmax(ex.data(), grp.first, grp.second);
      if (k != grp.first) feats |= morph::fromString(m.labels[static_cast<size_t>(k)]);
    }
    sentence[i].upos.assign(tag);
    sentence[i].feats = feats;
    t2 = t1;
    t1 = tag;
  }
}

}  // namespace vp::nlp
