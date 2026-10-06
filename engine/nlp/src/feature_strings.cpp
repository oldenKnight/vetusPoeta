// Mirror of tools/train/features.py. Keep the two files in the same order; the golden tests pin them together.
#include "feature_strings.h"

#include <string>

#include "vp/text.h"

namespace vp::nlp::detail {

const char* const kBos = "-BOS-";
const char* const kBos2 = "-BOS2-";
const char* const kEos = "-EOS-";
const char* const kEos2 = "-EOS2-";
const char* const kNone = "-NONE-";
const char* const kRoot = "-ROOT-";

namespace {
bool isCont(unsigned char c) { return (c & 0xC0) == 0x80; }
}  // namespace

std::string_view suffix(std::string_view s, size_t k) {
  size_t i = s.size();
  size_t cps = 0;
  while (i > 0 && cps < k) {
    --i;
    while (i > 0 && isCont(static_cast<unsigned char>(s[i]))) --i;
    ++cps;
  }
  return s.substr(i);
}

std::string_view prefix(std::string_view s, size_t k) {
  size_t i = 0;
  size_t cps = 0;
  while (i < s.size() && cps < k) {
    ++i;
    while (i < s.size() && isCont(static_cast<unsigned char>(s[i]))) ++i;
    ++cps;
  }
  return s.substr(0, i);
}

void shape(std::string_view w, std::string& out) {
  out.clear();
  std::string one, low;
  char32_t last = 0;
  size_t count = 0;
  size_t i = 0;
  while (i < w.size() && count < 6) {
    const size_t start = i;
    const char32_t ch = vp::text::decodeUtf8(w, i);
    char32_t c;
    one.assign(w.substr(start, i - start));
    if (ch >= '0' && ch <= '9') {
      c = 'd';
    } else {
      vp::text::lower(one, low);
      if (low != one && ch != vp::text::kReplacement) {
        c = 'X';
      } else if (ch >= 'a' && ch <= 'z') {
        c = 'x';
      } else if (ch < 128 || (ch >= 0xA1 && ch <= 0xBF) || (ch >= 0x2000 && ch <= 0x206F)) {
        c = ch;
      } else {
        c = 'x';
      }
    }
    if (c != last) {
      vp::text::appendUtf8(out, c);
      last = c;
      ++count;
    }
  }
}

void tagFeatures(const std::vector<std::string_view>& words, const std::vector<std::string_view>& lows,
                 const std::vector<std::string>& shapes, size_t i, std::string_view t1, std::string_view t2,
                 FeatBuf& out) {
  const size_t n = lows.size();
  const std::string_view w = words[i];
  const std::string_view lw = lows[i];
  const std::string_view lm1 = i >= 1 ? lows[i - 1] : std::string_view(kBos);
  const std::string_view lm2 = i >= 2 ? lows[i - 2] : std::string_view(i == 1 ? kBos : kBos2);
  const std::string_view lp1 = i + 1 < n ? lows[i + 1] : std::string_view(kEos);
  const std::string_view lp2 = i + 2 < n ? lows[i + 2] : std::string_view(i + 1 < n ? kEos : kEos2);
  const std::string_view sp = " ";
  out.add(std::string_view("b"));
  out.add(std::string_view("lw="), lw);
  out.add(std::string_view("s1="), suffix(lw, 1));
  out.add(std::string_view("s2="), suffix(lw, 2));
  out.add(std::string_view("s3="), suffix(lw, 3));
  out.add(std::string_view("p1="), prefix(lw, 1));
  out.add(std::string_view("sh="), std::string_view(shapes[i]));
  out.add(std::string_view("lw-1="), lm1);
  out.add(std::string_view("lw+1="), lp1);
  out.add(std::string_view("lw-2="), lm2);
  out.add(std::string_view("lw+2="), lp2);
  out.add(std::string_view("t-1="), t1);
  out.add(std::string_view("t-1t-2="), t1, sp, t2);
  out.add(std::string_view("lwt-1="), lw, sp, t1);
  out.add(std::string_view("s3-1="), i >= 1 ? suffix(lm1, 3) : std::string_view(kBos));
  out.add(std::string_view("s3+1="), i + 1 < n ? suffix(lp1, 3) : std::string_view(kEos));
  out.add(std::string_view("s4="), suffix(lw, 4));
  out.add(std::string_view("p2="), prefix(lw, 2));
  out.add(std::string_view("t-1lw+1="), t1, sp, lp1);
  out.add(std::string_view("lw-1lw="), lm1, sp, lw);
  out.add(std::string_view("lwlw+1="), lw, sp, lp1);
  out.add(std::string_view("p3="), prefix(lw, 3));
  if (w != lw) out.add(std::string_view("w="), w);
}

void tagFeatExtra(std::string_view upos, std::string_view lw, FeatBuf& out) {
  out.add(std::string_view("u="), upos);
  out.add(std::string_view("us2="), upos, std::string_view(" "), suffix(lw, 2));
}

}  // namespace vp::nlp::detail

namespace vp::nlp::detail {

void ParseState::reset(int words) {
  const size_t m = static_cast<size_t>(words) + 1;
  n = words;
  stack.clear();
  stack.push_back(0);
  b = 1;
  heads.assign(m, -1);
  lc1.assign(m, -1);
  lc2.assign(m, -1);
  rc1.assign(m, -1);
  rc2.assign(m, -1);
  nl.assign(m, 0);
  nr.assign(m, 0);
  labels.assign(m, std::string_view());
}

void ParseState::arc(int h, int d, std::string_view label) {
  const size_t H = static_cast<size_t>(h), D = static_cast<size_t>(d);
  heads[D] = h;
  labels[D] = label;
  if (d < h) {
    nl[H] += 1;
    if (lc1[H] < 0 || d < lc1[H]) {
      lc2[H] = lc1[H];
      lc1[H] = d;
    } else if (lc2[H] < 0 || d < lc2[H]) {
      lc2[H] = d;
    }
  } else {
    nr[H] += 1;
    if (rc1[H] < 0 || d > rc1[H]) {
      rc2[H] = rc1[H];
      rc1[H] = d;
    } else if (rc2[H] < 0 || d > rc2[H]) {
      rc2[H] = d;
    }
  }
}

std::string_view distBucket(int d) {
  static const char* const small[] = {"0", "1", "2", "3", "4"};
  if (d >= 0 && d < 5) return small[d];
  return d < 10 ? "5" : "10";   // negative distances do not occur
}

namespace {
inline std::string_view at(const std::vector<std::string_view>& v, int i) {
  return i >= 0 ? v[static_cast<size_t>(i)] : std::string_view(kNone);
}
inline std::string_view lab(const std::vector<std::string_view>& labels, int i) {
  if (i < 0) return kNone;
  const std::string_view s = labels[static_cast<size_t>(i)];
  return s;   // only called for attached children, whose label is set
}
}  // namespace

void parseFeatures(const std::vector<std::string_view>& lows, const std::vector<std::string_view>& tags,
                   const ParseState& st, FeatBuf& out) {
  const std::string_view none(kNone);
  std::string_view s0w = none, s0p = none, s0hp = none, s0hw = none, s0l = none, s0lp = none, s0ll = none,
                   s0rp = none, s0rl = none, s0l2p = none, s0r2p = none, s0vl = "0", s0vr = "0";
  std::string vlBuf, vrBuf, n0vlBuf;
  int s0 = -1;
  if (!st.stack.empty()) {
    s0 = st.stack.back();
    const size_t S = static_cast<size_t>(s0);
    s0w = lows[S];
    s0p = tags[S];
    const int h = st.heads[S];
    s0hp = at(tags, h);
    s0hw = at(lows, h);
    s0l = st.labels[S].empty() ? none : st.labels[S];
    s0lp = at(tags, st.lc1[S]);
    s0ll = lab(st.labels, st.lc1[S]);
    s0rp = at(tags, st.rc1[S]);
    s0rl = lab(st.labels, st.rc1[S]);
    s0l2p = at(tags, st.lc2[S]);
    s0r2p = at(tags, st.rc2[S]);
    vlBuf = std::to_string(st.nl[S]);
    vrBuf = std::to_string(st.nr[S]);
    s0vl = vlBuf;
    s0vr = vrBuf;
  }
  std::string_view s1w = none, s1p = none;
  if (st.stack.size() >= 2) {
    const size_t S1 = static_cast<size_t>(st.stack[st.stack.size() - 2]);
    s1w = lows[S1];
    s1p = tags[S1];
  }
  const int b = st.b, n = st.n;
  std::string_view n0w = none, n0p = none, n0lp = none, n0ll = none, n0l2p = none, n0vl = "0";
  if (b <= n) {
    const size_t B = static_cast<size_t>(b);
    n0w = lows[B];
    n0p = tags[B];
    n0lp = at(tags, st.lc1[B]);
    n0ll = lab(st.labels, st.lc1[B]);
    n0l2p = at(tags, st.lc2[B]);
    n0vlBuf = std::to_string(st.nl[B]);
    n0vl = n0vlBuf;
  }
  const std::string_view n1w = b + 1 <= n ? lows[static_cast<size_t>(b + 1)] : none;
  const std::string_view n1p = b + 1 <= n ? tags[static_cast<size_t>(b + 1)] : none;
  const std::string_view n2p = b + 2 <= n ? tags[static_cast<size_t>(b + 2)] : none;
  const std::string_view d = (s0 >= 0 && b <= n) ? distBucket(b - s0) : std::string_view("0");
  const std::string_view sp = " ";
  using sv = std::string_view;
  out.add(sv("b"));
  out.add(sv("s0w="), s0w);
  out.add(sv("s0p="), s0p);
  out.add(sv("s0wp="), s0w, sp, s0p);
  out.add(sv("n0w="), n0w);
  out.add(sv("n0p="), n0p);
  out.add(sv("n0wp="), n0w, sp, n0p);
  out.add(sv("n1w="), n1w);
  out.add(sv("n1p="), n1p);
  out.add(sv("n2p="), n2p);
  out.add(sv("s1p="), s1p);
  out.add(sv("s1w="), s1w);
  out.add(sv("s0wp,n0p="), s0w, sp, s0p, sp, n0p);
  out.add(sv("s0p,n0wp="), s0p, sp, n0w, sp, n0p);
  out.add(sv("s0w,n0w="), s0w, sp, n0w);
  out.add(sv("s0p,n0p="), s0p, sp, n0p);
  out.add(sv("n0p,n1p="), n0p, sp, n1p);
  out.add(sv("n0p,n1p,n2p="), n0p, sp, n1p, sp, n2p);
  out.add(sv("s0p,n0p,n1p="), s0p, sp, n0p, sp, n1p);
  out.add(sv("s1p,s0p,n0p="), s1p, sp, s0p, sp, n0p);
  out.add(sv("s0hp,s0p,n0p="), s0hp, sp, s0p, sp, n0p);
  out.add(sv("s0p,s0lp,n0p="), s0p, sp, s0lp, sp, n0p);
  out.add(sv("s0p,s0rp,n0p="), s0p, sp, s0rp, sp, n0p);
  out.add(sv("s0p,n0p,n0lp="), s0p, sp, n0p, sp, n0lp);
  out.add(sv("s0p,s0lp,s0l2p="), s0p, sp, s0lp, sp, s0l2p);
  out.add(sv("s0p,s0rp,s0r2p="), s0p, sp, s0rp, sp, s0r2p);
  out.add(sv("n0p,n0lp,n0l2p="), n0p, sp, n0lp, sp, n0l2p);
  out.add(sv("s0w,d="), s0w, sp, d);
  out.add(sv("n0w,d="), n0w, sp, d);
  out.add(sv("s0p,n0p,d="), s0p, sp, n0p, sp, d);
  out.add(sv("s0p,vl="), s0p, sp, s0vl);
  out.add(sv("s0p,vr="), s0p, sp, s0vr);
  out.add(sv("n0p,vl="), n0p, sp, n0vl);
  out.add(sv("s0hw="), s0hw);
  out.add(sv("s0l="), s0l);
  out.add(sv("s0ll,s0p="), s0ll, sp, s0p);
  out.add(sv("s0rl,s0p="), s0rl, sp, s0p);
  out.add(sv("n0ll,n0p="), n0ll, sp, n0p);
}

void labelFeatures(const std::vector<std::string_view>& lows, const std::vector<std::string_view>& tags, int h,
                   int d, std::string_view dir, const ParseState& st, FeatBuf& out) {
  const size_t H = static_cast<size_t>(h), D = static_cast<size_t>(d);
  const std::string_view hw = lows[H], hp = tags[H], dw = lows[D], dp = tags[D];
  const std::string_view dll = lab(st.labels, st.lc1[D]);
  const std::string_view drl = lab(st.labels, st.rc1[D]);
  const std::string_view dist = h != 0 ? distBucket(h > d ? h - d : d - h) : std::string_view("r");
  const std::string_view sp = " ";
  using sv = std::string_view;
  out.add(sv("Lb="), dir);
  out.add(sv("Ldp="), dir, sp, dp);
  out.add(sv("Lhp,dp="), dir, sp, hp, sp, dp);
  out.add(sv("Ldw="), dir, sp, dw);
  out.add(sv("Lhw,dp="), dir, sp, hw, sp, dp);
  out.add(sv("Lhp,dw="), dir, sp, hp, sp, dw);
  out.add(sv("Ldp,dist="), dir, sp, dp, sp, dist);
  out.add(sv("Ldp,dll,drl="), dir, sp, dp, sp, dll, sp, drl);
}

}  // namespace vp::nlp::detail
