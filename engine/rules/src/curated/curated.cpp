// Loaders for data/curated/*.tsv and order_la.txt (see vp/curated.h).
#include "vp/curated.h"

#include <algorithm>

#include "vp/features.h"
#include "vp/fs.h"
#include "vp/text.h"

namespace vp::curated {

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\r' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

void splitTabs(std::string_view line, std::vector<std::string_view>& cols) {
  cols.clear();
  size_t start = 0;
  for (;;) {
    size_t t = line.find('\t', start);
    if (t == std::string_view::npos) { cols.push_back(trim(line.substr(start))); break; }
    cols.push_back(trim(line.substr(start, t - start)));
    start = t + 1;
  }
}

// Calls fn(lineNo, line) for every non-comment, non-blank line.
template <class Fn> void forLines(std::string_view data, Fn&& fn) {
  int no = 0;
  size_t pos = 0;
  if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
      (unsigned char)data[2] == 0xBF)
    pos = 3;
  while (pos <= data.size()) {
    size_t nl = data.find('\n', pos);
    std::string_view line = data.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    ++no;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    std::string_view t = trim(line);
    if (!t.empty() && t.front() != '#') fn(no, line);
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
}

template <class T> void sortByKey(std::vector<T>& v) {
  std::stable_sort(v.begin(), v.end(), [](const T& a, const T& b) { return a.key < b.key; });
}
template <class T> const T* findByKey(const std::vector<T>& v, std::string_view key) {
  auto it = std::lower_bound(v.begin(), v.end(), key, [](const T& a, std::string_view k) { return a.key < k; });
  if (it != v.end() && it->key == key) return &*it;
  return nullptr;
}

bool isSlotName(std::string_view s) {
  static const char* const k[] = {"VOC", "CONN", "S", "IO", "O", "OBL", "ADV", "NEG", "V", "PRED", "INF", "WH", "Q"};
  for (const char* n : k)
    if (s == n) return true;
  return false;
}

}  // namespace

uint8_t parseCase(std::string_view s) {
  if (s == "nom") return feat::Nom;
  if (s == "gen") return feat::Gen;
  if (s == "dat") return feat::Dat;
  if (s == "acc") return feat::Acc;
  if (s == "abl") return feat::Abl;
  if (s == "voc") return feat::Voc;
  if (s == "loc") return feat::Loc;
  return 0;
}

Frame parseFrame(std::string_view s) {
  Frame f;
  f.raw = std::string(s);
  if (s == "acc") f.kind = FrameKind::Acc;
  else if (s == "dat") f.kind = FrameKind::Dat;
  else if (s == "abl") f.kind = FrameKind::Abl;
  else if (s == "gen") f.kind = FrameKind::Gen;
  else if (s == "dat+acc") f.kind = FrameKind::DatAcc;
  else if (s == "acc+inf") f.kind = FrameKind::AccInf;
  else if (s == "inf") f.kind = FrameKind::Inf;
  else if (s == "ut") f.kind = FrameKind::Ut;
  else if (s == "nē" || s == "ne") f.kind = FrameKind::Ne;
  else if (s == "quod") f.kind = FrameKind::Quod;
  else if (s == "intr") f.kind = FrameKind::Intr;
  else if (s == "copula") f.kind = FrameKind::Copula;
  else if (s == "refl") f.kind = FrameKind::Refl;
  else if (s.substr(0, 7) == "impers:") {
    f.kind = FrameKind::Impers;
    f.impers = std::string(s.substr(7));
  } else if (s.substr(0, 5) == "prep:") {
    std::string_view rest = s.substr(5);
    size_t plus = rest.find('+');
    if (plus != std::string_view::npos) {
      f.kind = FrameKind::Prep;
      f.prep = text::latin_key(rest.substr(0, plus));
      f.prepCase = parseCase(rest.substr(plus + 1));
    }
  }
  return f;
}

struct Loader {
  CuratedData& d;
  std::string file;
  std::vector<std::string_view> cols;
  void warn(int line, std::string msg) { d.warnings_.push_back(LoadWarning{file, line, std::move(msg)}); }
  bool need(int line, size_t n) {
    if (cols.size() >= n && !cols[0].empty()) return true;
    warn(line, "expected at least " + std::to_string(n) + " tab-separated columns, got " + std::to_string(cols.size()));
    return false;
  }
  static std::string col(const std::vector<std::string_view>& c, size_t i) {
    return i < c.size() ? std::string(c[i]) : std::string();
  }
  uint8_t tierOf(int line, std::string_view s) {
    if (s == "1") return 1;
    if (s == "2") return 2;
    if (s == "3") return 3;
    if (s == "0") return 0;
    warn(line, "tier must be 0..3, got '" + std::string(s) + "'");
    return 0;
  }

  void valency(std::string_view data) {
    forLines(data, [&](int no, std::string_view line) {
      splitTabs(line, cols);
      if (!need(no, 2)) return;
      Valency v;
      v.key = text::latin_key(cols[0]);
      // homograph markers ("volo2" = volāre) keep their digit so they do not collide with the main entry
      if (!cols[0].empty() && cols[0].back() >= '0' && cols[0].back() <= '9') v.key += cols[0].back();
      v.example = col(cols, 2);
      v.note = col(cols, 3);
      std::string_view fr = cols[1];
      size_t p = 0;
      while (p <= fr.size()) {
        size_t semi = fr.find(';', p);
        std::string_view one = trim(fr.substr(p, semi == std::string_view::npos ? std::string_view::npos : semi - p));
        if (!one.empty()) {
          Frame f = parseFrame(one);
          if (f.kind == FrameKind::Other) warn(no, "unknown frame '" + std::string(one) + "'");
          v.frames.push_back(std::move(f));
        }
        if (semi == std::string_view::npos) break;
        p = semi + 1;
      }
      // "acc;acc" (double accusative) and "acc;abl" (price, material) are frame pairs: record them as such too.
      if (v.frames.size() == 2 && v.frames[0].kind == FrameKind::Acc && v.frames[1].kind == FrameKind::Acc) {
        Frame f; f.kind = FrameKind::AccAcc; f.raw = "acc+acc"; v.frames.push_back(f);
      }
      if (v.frames.empty()) { warn(no, "no frame"); return; }
      d.valency_.push_back(std::move(v));
    });
  }
  void names(std::string_view data) {
    forLines(data, [&](int no, std::string_view line) {
      splitTabs(line, cols);
      if (!need(no, 6)) return;
      NameEntry n;
      n.english = col(cols, 0);
      n.latinNom = text::nfc(cols[1]);
      n.latinGen = cols[2] == "-" ? std::string() : text::nfc(cols[2]);
      std::string_view g = cols[3];
      n.gender = g == "m" ? feat::M : g == "f" ? feat::F : g == "n" ? feat::N : 0;
      if (!n.gender) warn(no, "gender must be m, f or n");
      std::string_view dcl = cols[4];
      if (dcl == "1" || dcl == "2" || dcl == "3") n.declension = dcl[0] - '0';
      else if (dcl == "indecl") n.declension = 0;
      else if (dcl == "-") n.declension = -1;
      else { warn(no, "declension must be 1, 2, 3, indecl or -"); n.declension = 0; }
      std::string_view pol = cols[5];
      if (pol == "keep") n.policy = NamePolicy::Keep;
      else if (pol == "decline") n.policy = NamePolicy::Decline;
      else if (pol == "translate") n.policy = NamePolicy::Translate;
      else { warn(no, "policy must be keep, decline or translate"); return; }
      n.note = col(cols, 6);
      d.names_.push_back(std::move(n));
    });
  }
  void tiers(std::string_view data, std::vector<TierEntry>& out, bool greek) {
    forLines(data, [&](int no, std::string_view line) {
      splitTabs(line, cols);
      if (!need(no, 4)) return;
      TierEntry t;
      t.key = greek ? text::greek_key(cols[0]) : text::latin_key(cols[0]);
      t.head = text::nfc(cols[1]);
      t.pos = col(cols, 2);
      t.tier = tierOf(no, cols[3]);
      t.source = col(cols, 4);
      t.note = col(cols, 5);
      out.push_back(std::move(t));
    });
  }
  void emoji(std::string_view data, std::vector<EmojiEntry>& out, bool greek) {
    forLines(data, [&](int no, std::string_view line) {
      splitTabs(line, cols);
      if (!need(no, 3) || cols[2].empty()) { if (cols.size() >= 3 && cols[2].empty()) warn(no, "empty emoji"); return; }
      EmojiEntry e;
      e.key = greek ? text::greek_key(cols[0]) : text::latin_key(cols[0]);
      e.head = text::nfc(cols[1]);
      e.emoji = col(cols, 2);
      e.note = col(cols, 3);
      out.push_back(std::move(e));
    });
  }
  void periphrasis(std::string_view data) {
    forLines(data, [&](int no, std::string_view line) {
      splitTabs(line, cols);
      if (!need(no, 3)) return;
      PeriphrasisEntry p;
      p.key = text::latin_key(cols[0]);
      p.tier = tierOf(no, cols[1]);
      p.periphrasis = text::nfc(cols[2]);
      p.note = col(cols, 3);
      d.periphrasis_.push_back(std::move(p));
    });
  }
  void preps(std::string_view data) {
    forLines(data, [&](int no, std::string_view line) {
      splitTabs(line, cols);
      if (!need(no, 4)) return;
      PrepEntry p;
      p.english = col(cols, 0);
      p.context = col(cols, 1);
      p.latin = text::nfc(cols[2]);
      p.latinKey = p.latin == "-" ? std::string() : text::latin_key(p.latin);
      p.caseRaw = col(cols, 3);
      p.case_ = parseCase(cols[3]);
      p.infinitive = cols[3] == "inf";
      if (!p.case_ && !p.infinitive && cols[3] != "-") warn(no, "unknown case '" + p.caseRaw + "'");
      p.note = col(cols, 4);
      d.preps_.push_back(std::move(p));
    });
  }
  void phrasebook(std::string_view data) {
    forLines(data, [&](int no, std::string_view line) {
      splitTabs(line, cols);
      if (!need(no, 2)) return;
      PhraseEntry p;
      p.pattern = col(cols, 0);
      p.latin = text::nfc(cols[1]);
      p.tier = cols.size() > 2 && !cols[2].empty() ? tierOf(no, cols[2]) : 0;
      p.reg = col(cols, 3);
      p.note = col(cols, 4);
      d.phrasebook_.push_back(std::move(p));
    });
  }
  void pairs(std::string_view data, std::vector<PairEntry>& out) {
    forLines(data, [&](int no, std::string_view line) {
      splitTabs(line, cols);
      if (!need(no, 2) || cols[1].empty()) { if (cols.size() >= 2 && cols[1].empty()) warn(no, "empty second column"); return; }
      out.push_back(PairEntry{col(cols, 0), text::nfc(cols[1])});
    });
  }
  void glossEs(std::string_view data) {
    forLines(data, [&](int no, std::string_view line) {
      splitTabs(line, cols);
      if (!need(no, 3)) return;
      d.glossEs_.push_back(GlossEsEntry{text::latin_key(cols[0]), text::nfc(cols[1]), col(cols, 2)});
    });
  }
  void order(std::string_view data) {
    forLines(data, [&](int no, std::string_view line) {
      std::string_view t = trim(line);
      if (t.substr(0, 5) != "RULE ") { warn(no, "line does not start with RULE"); return; }
      t.remove_prefix(5);
      size_t colon = t.find(" : ");
      size_t arrow = t.find("=>");
      if (colon == std::string_view::npos || arrow == std::string_view::npos || arrow < colon) {
        warn(no, "expected 'RULE <id> : <condition> => <ordering> ; <note>'");
        return;
      }
      OrderRule r;
      r.line = no;
      r.id = std::string(trim(t.substr(0, colon)));
      r.conditionText = std::string(trim(t.substr(colon + 3, arrow - colon - 3)));
      std::string_view rest = t.substr(arrow + 2);
      size_t semi = rest.find(';');
      r.orderingText = std::string(trim(rest.substr(0, semi)));
      if (semi != std::string_view::npos) r.note = std::string(trim(rest.substr(semi + 1)));
      auto words = [](const std::string& s, std::vector<std::string>& out) {
        size_t p = 0;
        while (p < s.size()) {
          while (p < s.size() && s[p] == ' ') ++p;
          size_t e = s.find(' ', p);
          if (e == std::string::npos) e = s.size();
          if (e > p) out.emplace_back(s.substr(p, e - p));
          p = e;
        }
      };
      words(r.conditionText, r.condition);
      words(r.orderingText, r.ordering);
      if (r.id.empty() || r.ordering.empty()) { warn(no, "empty rule id or ordering"); return; }
      for (const OrderRule& o : d.order_)
        if (o.id == r.id) { warn(no, "duplicate rule id " + r.id); return; }
      d.order_.push_back(std::move(r));
    });
  }
};

Result<CuratedData> CuratedData::load(const std::filesystem::path& dir) {
  CuratedData d;
  Loader L{d, {}, {}};
  struct FileSpec { const char* name; int kind; };
  static const FileSpec files[] = {
      {"valency_la.tsv", 0}, {"names_la.tsv", 1},       {"tiers_la.tsv", 2},         {"tiers_grc.tsv", 3},
      {"emoji_la.tsv", 4},   {"emoji_grc.tsv", 5},      {"periphrasis_la.tsv", 6},   {"preps_en_la.tsv", 7},
      {"phrasebook_en_la.tsv", 8}, {"contractions_en.tsv", 9}, {"nonverbal_en_la.tsv", 10}, {"gloss_es_la.tsv", 11},
      {"order_la.txt", 12}};
  for (const FileSpec& f : files) {
    const std::string path = fs::toU8(dir / f.name);
    Result<std::string> data = fs::readFile(path, 16u << 20);
    if (!data.ok()) {
      const bool missing = data.error().code == ErrorCode::NotFound;
      return Error{missing ? ErrorCode::NotFound : ErrorCode::Io,
                   std::string("curated data: cannot read ") + path + ": " + data.error().message,
                   missing ? std::string("The file data/curated/") + f.name +
                                 " is missing. Reinstall the program or point the data folder at a complete copy."
                           : std::string("The file data/curated/") + f.name + " could not be read: " +
                                 data.error().hint};
    }
    L.file = f.name;
    const std::string& s = data.value();
    if (!text::isValidUtf8(s)) L.warn(0, "invalid UTF-8 (bad bytes read as U+FFFD)");
    switch (f.kind) {
      case 0: L.valency(s); break;
      case 1: L.names(s); break;
      case 2: L.tiers(s, d.tiers_, false); break;
      case 3: L.tiers(s, d.tiersGrc_, true); break;
      case 4: L.emoji(s, d.emoji_, false); break;
      case 5: L.emoji(s, d.emojiGrc_, true); break;
      case 6: L.periphrasis(s); break;
      case 7: L.preps(s); break;
      case 8: L.phrasebook(s); break;
      case 9: L.pairs(s, d.contractions_); break;
      case 10: L.pairs(s, d.nonverbal_); break;
      case 11: L.glossEs(s); break;
      default: L.order(s); break;
    }
  }
  // Sort keyed tables; record duplicates (the first row wins).
  auto dedupe = [&](auto& v, const char* file) {
    sortByKey(v);
    for (size_t i = 1; i < v.size(); ++i)
      if (v[i].key == v[i - 1].key) d.warnings_.push_back(LoadWarning{file, 0, "duplicate key " + v[i].key + " (first row wins)"});
  };
  dedupe(d.valency_, "valency_la.tsv");
  auto dedupeTiers = [&](std::vector<TierEntry>& v, const char* file) {   // key + pos is the identity
    std::stable_sort(v.begin(), v.end(), [](const TierEntry& a, const TierEntry& b) { return a.key < b.key; });
    for (size_t i = 1; i < v.size(); ++i)
      for (size_t j = i; j-- > 0 && v[j].key == v[i].key;)
        if (v[j].pos == v[i].pos) {
          d.warnings_.push_back(LoadWarning{file, 0, "duplicate key " + v[i].key + " (" + v[i].pos + ", first row wins)"});
          break;
        }
  };
  dedupeTiers(d.tiers_, "tiers_la.tsv");
  dedupeTiers(d.tiersGrc_, "tiers_grc.tsv");
  dedupe(d.emoji_, "emoji_la.tsv");
  dedupe(d.emojiGrc_, "emoji_grc.tsv");
  dedupe(d.periphrasis_, "periphrasis_la.tsv");
  dedupe(d.glossEs_, "gloss_es_la.tsv");
  for (uint32_t i = 0; i < d.names_.size(); ++i) d.namesByLatin_.push_back(i);
  d.namesLatinKey_.reserve(d.names_.size());
  for (const NameEntry& n : d.names_) d.namesLatinKey_.push_back(text::latin_key(n.latinNom));
  std::stable_sort(d.namesByLatin_.begin(), d.namesByLatin_.end(),
                   [&](uint32_t a, uint32_t b) { return d.namesLatinKey_[a] < d.namesLatinKey_[b]; });
  for (const PrepEntry& p : d.preps_) {
    if (p.latinKey.empty() || !p.case_ || p.latinKey.find(' ') != std::string::npos) continue;
    auto add = [&](const std::string& k) {
      auto it = std::find_if(d.prepCases_.begin(), d.prepCases_.end(), [&](auto& e) { return e.first == k; });
      if (it == d.prepCases_.end()) d.prepCases_.emplace_back(k, (uint16_t)(1u << p.case_));
      else it->second |= (uint16_t)(1u << p.case_);
    };
    add(p.latinKey);
    // ā/ab and ē/ex are one preposition each (note column of the table); sub + acc for motion (note of "under").
    if (p.latinKey == "a") add("ab");
    if (p.latinKey == "e") add("ex");
    if (p.latinKey == "sub") { auto it = std::find_if(d.prepCases_.begin(), d.prepCases_.end(), [](auto& e) { return e.first == "sub"; }); it->second |= (uint16_t)(1u << feat::Acc); }
  }
  std::sort(d.prepCases_.begin(), d.prepCases_.end());
  for (const char* req : {"order.decl", "order.copula", "order.yn", "order.wh", "order.imp", "order.adj"})
    if (!d.rule(req)) d.warnings_.push_back(LoadWarning{"order_la.txt", 0, std::string("rule ") + req + " missing; built-in default used"});
  return d;
}

const Valency* CuratedData::valency(std::string_view key) const { return findByKey(valency_, key); }
const TierEntry* CuratedData::tier(std::string_view key) const { return findByKey(tiers_, key); }
const TierEntry* CuratedData::tier(std::string_view key, std::string_view pos) const {
  auto it = std::lower_bound(tiers_.begin(), tiers_.end(), key, [](const TierEntry& a, std::string_view k) { return a.key < k; });
  for (; it != tiers_.end() && it->key == key; ++it)
    if (it->pos == pos) return &*it;
  return tier(key);
}
const TierEntry* CuratedData::tierGreek(std::string_view key) const { return findByKey(tiersGrc_, key); }
const EmojiEntry* CuratedData::emoji(std::string_view key) const { return findByKey(emoji_, key); }
const EmojiEntry* CuratedData::emojiGreek(std::string_view key) const { return findByKey(emojiGrc_, key); }
const PeriphrasisEntry* CuratedData::periphrasis(std::string_view key) const { return findByKey(periphrasis_, key); }
const GlossEsEntry* CuratedData::glossEs(std::string_view key) const { return findByKey(glossEs_, key); }

const NameEntry* CuratedData::nameByEnglish(std::string_view english) const {
  const std::string k = text::en_key(english);
  for (const NameEntry& n : names_)
    if (text::en_key(n.english) == k) return &n;
  return nullptr;
}
const NameEntry* CuratedData::nameByLatin(std::string_view latinKey) const {
  auto it = std::lower_bound(namesByLatin_.begin(), namesByLatin_.end(), latinKey,
                             [&](uint32_t i, std::string_view k) { return namesLatinKey_[i] < k; });
  if (it != namesByLatin_.end() && namesLatinKey_[*it] == latinKey) return &names_[*it];
  return nullptr;
}
uint16_t CuratedData::prepCases(std::string_view latinKey) const {
  auto it = std::lower_bound(prepCases_.begin(), prepCases_.end(), latinKey,
                             [](const std::pair<std::string, uint16_t>& e, std::string_view k) { return e.first < k; });
  if (it != prepCases_.end() && it->first == latinKey) return it->second;
  return 0;
}
const OrderRule* CuratedData::rule(std::string_view id) const {
  for (const OrderRule& r : order_)
    if (r.id == id) return &r;
  return nullptr;
}

std::vector<std::string> CuratedData::conditionSet(std::string_view ruleId) const {
  std::vector<std::string> out;
  const OrderRule* r = rule(ruleId);
  if (!r) return out;
  const std::string& c = r->conditionText;
  size_t a = c.find('{'), b = c.find('}');
  if (a == std::string::npos || b == std::string::npos || b < a) return out;
  std::string_view inner(c.data() + a + 1, b - a - 1);
  size_t p = 0;
  while (p <= inner.size()) {
    size_t comma = inner.find(',', p);
    std::string_view w = trim(inner.substr(p, comma == std::string_view::npos ? std::string_view::npos : comma - p));
    if (!w.empty()) out.push_back(text::latin_key(w));
    if (comma == std::string_view::npos) break;
    p = comma + 1;
  }
  return out;
}

std::vector<std::string> CuratedData::orderingList(std::string_view ruleId, std::string_view marker) const {
  std::vector<std::string> out;
  const OrderRule* r = rule(ruleId);
  if (!r) return out;
  const std::string o = r->orderingText + " ; " + r->note;
  size_t m = marker.empty() ? 0 : o.find(marker);
  if (m == std::string::npos) return out;
  size_t a = o.find('(', m);
  size_t b = a == std::string::npos ? std::string::npos : o.find(')', a);
  std::string_view inner;
  if (a != std::string::npos && b != std::string::npos) inner = std::string_view(o.data() + a + 1, b - a - 1);
  else {  // a plain comma list after the marker up to the end ("... become enclitic: mēcum, tēcum, ...")
    size_t colon = o.find(':', m);
    if (colon == std::string::npos) return out;
    inner = std::string_view(o.data() + colon + 1, o.size() - colon - 1);
  }
  size_t p = 0;
  while (p <= inner.size()) {
    size_t comma = inner.find(',', p);
    std::string_view w = trim(inner.substr(p, comma == std::string_view::npos ? std::string_view::npos : comma - p));
    if (!w.empty() && w.find(' ') == std::string_view::npos) out.push_back(text::latin_key(w));
    if (comma == std::string_view::npos) break;
    p = comma + 1;
  }
  return out;
}

std::vector<std::string> CuratedData::slotTemplate(std::string_view ruleId) const {
  std::vector<std::string> out;
  const OrderRule* r = rule(ruleId);
  if (!r) return out;
  for (const std::string& tok : r->ordering) {
    std::string s;
    for (char ch : tok)
      if (ch != '[' && ch != ']' && ch != ',') s += ch;
    if (s == "...") continue;
    if (!isSlotName(s)) break;
    out.push_back(s);
  }
  return out;
}

}  // namespace vp::curated
