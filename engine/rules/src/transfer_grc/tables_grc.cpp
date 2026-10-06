// GreekTables (lexical_en_grc.tsv, tiers_grc.tsv glosses, phrasebook_es_grc.tsv) and the Greek cue helpers
// (break hints, sentence capitals, monotonic export). See vp/transfer_grc.h.
#include <algorithm>
#include <fstream>

#include "vp/morph_grc.h"
#include "vp/text.h"
#include "vp/transfer_grc.h"

namespace vp::grc {

namespace {

std::vector<std::string> splitTabs(const std::string& line) {
  std::vector<std::string> cols;
  size_t s = 0;
  for (;;) {
    const size_t t = line.find('\t', s);
    cols.push_back(line.substr(s, t == std::string::npos ? std::string::npos : t - s));
    if (t == std::string::npos) break;
    s = t + 1;
  }
  for (std::string& c : cols) {
    while (!c.empty() && (c.back() == ' ' || c.back() == '\r')) c.pop_back();
    size_t a = 0;
    while (a < c.size() && c[a] == ' ') ++a;
    c.erase(0, a);
  }
  return cols;
}

bool readLines(const std::filesystem::path& p, std::vector<std::string>& out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out.push_back(line);
  }
  return true;
}

// Gloss items of a note: "come back, return; go" -> {"come back", "return", "go"} (lower case, articles and "to"
// dropped, parentheses removed).
std::vector<std::string> glossItems(const std::string& note) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  auto flush = [&]() {
    std::string w = text::lower(cur);
    cur.clear();
    while (!w.empty() && w.back() == ' ') w.pop_back();
    size_t a = 0;
    while (a < w.size() && w[a] == ' ') ++a;
    w.erase(0, a);
    for (const char* p : {"to ", "a ", "an ", "the "})
      if (w.compare(0, std::char_traits<char>::length(p), p) == 0) w.erase(0, std::char_traits<char>::length(p));
    if (!w.empty() && w.find('"') == std::string::npos) out.push_back(w);
  };
  for (char ch : note) {
    if (ch == '(') { ++depth; continue; }
    if (ch == ')') { if (depth) --depth; continue; }
    if (depth) continue;
    if (ch == ',' || ch == ';') flush();
    else cur += ch;
  }
  flush();
  return out;
}

}  // namespace

Result<GreekTables> GreekTables::load(const std::filesystem::path& dir) {
  GreekTables t;
  std::vector<std::string> lines;
  if (!readLines(dir / "lexical_en_grc.tsv", lines))
    return Result<GreekTables>(ErrorCode::NotFound, "lexical_en_grc.tsv not found in " + dir.string(),
                               "The Greek rule tables (data/curated/lexical_en_grc.tsv) are missing.");
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& l = lines[i];
    if (l.empty() || l[0] == '#') continue;
    std::vector<std::string> c = splitTabs(l);
    if (c.size() < 3 || c[0].empty() || c[1].empty() || c[2].empty()) {
      t.warnings_.push_back({"lexical_en_grc.tsv", (int)i + 1, "expected kind, source, greek"});
      continue;
    }
    c.resize(5);
    t.rows_.push_back(LexRow{c[0], text::nfc(text::lower(c[1])), text::nfc(c[2]), c[3], c[4]});
  }
  lines.clear();
  if (readLines(dir / "tiers_grc.tsv", lines)) {
    for (size_t i = 0; i < lines.size(); ++i) {
      const std::string& l = lines[i];
      if (l.empty() || l[0] == '#') continue;
      std::vector<std::string> c = splitTabs(l);
      if (c.size() < 6 || c[5].empty()) continue;
      TaughtGloss g;
      g.key = text::greek_key(c[1]);
      g.head = text::nfc(c[1]);
      g.pos = c[2];
      g.tier = (uint8_t)std::max(0, std::min(3, std::atoi(c[3].c_str())));
      t.taught_.push_back(g);
      for (const std::string& item : glossItems(c[5])) t.taughtIndex_.emplace_back(item, (uint32_t)(t.taught_.size() - 1));
    }
    std::stable_sort(t.taughtIndex_.begin(), t.taughtIndex_.end(),
                     [](const std::pair<std::string, uint32_t>& a, const std::pair<std::string, uint32_t>& b) {
                       return a.first < b.first;
                     });
  }
  lines.clear();
  if (readLines(dir / "phrasebook_es_grc.tsv", lines)) {
    for (size_t i = 0; i < lines.size(); ++i) {
      const std::string& l = lines[i];
      if (l.empty() || l[0] == '#') continue;
      std::vector<std::string> c = splitTabs(l);
      if (c.size() < 2 || c[0].empty() || c[1].empty()) {
        t.warnings_.push_back({"phrasebook_es_grc.tsv", (int)i + 1, "expected pattern, greek"});
        continue;
      }
      c.resize(5);
      curated::PhraseEntry e;
      e.pattern = text::nfc(c[0]);
      e.latin = text::nfc(c[1]);
      e.tier = (uint8_t)std::max(0, std::min(3, std::atoi(c[2].c_str())));
      e.reg = c[3];
      e.note = c[4];
      t.phrasebookEs_.push_back(std::move(e));
    }
  } else {
    t.warnings_.push_back({"phrasebook_es_grc.tsv", 0, "file missing: no Spanish phrasebook for Greek"});
  }
  lines.clear();
  if (readLines(dir / "readable_grc.tsv", lines)) {
    for (size_t i = 0; i < lines.size(); ++i) {
      const std::string& l = lines[i];
      if (l.empty() || l[0] == '#') continue;
      std::vector<std::string> c = splitTabs(l);
      if (c.size() < 4 || c[0].empty() || c[1].empty()) {
        t.warnings_.push_back({"readable_grc.tsv", (int)i + 1, "expected head, pos, english, spanish"});
        continue;
      }
      c.resize(6);
      t.readable_.push_back(ReadableRow{text::greek_key(c[0]), text::nfc(c[0]), c[1], c[2], text::nfc(c[3]),
                                        c[4] == "-" ? std::string() : c[4], c[5]});
    }
    std::stable_sort(t.readable_.begin(), t.readable_.end(),
                     [](const ReadableRow& a, const ReadableRow& b) { return a.key < b.key; });
    for (const ReadableRow& r : t.readable_) {
      if (r.pos != "noun" && r.pos != "verb" && r.pos != "adj" && r.pos != "adv" && r.pos != "mid") continue;
      TaughtGloss g;
      g.key = r.key;
      g.head = r.head;
      g.pos = r.pos == "mid" ? "verb" : r.pos;
      t.readableTaught_.push_back(g);
      const uint32_t at = (uint32_t)(t.readableTaught_.size() - 1);
      if (!r.en.empty() && r.en != "-") t.readableIndex_.emplace_back("en:" + text::lower(r.en), at);
      if (!r.es.empty() && r.es != "-") t.readableIndex_.emplace_back("es:" + text::nfc(text::lower(r.es)), at);
    }
    std::stable_sort(t.readableIndex_.begin(), t.readableIndex_.end(),
                     [](const std::pair<std::string, uint32_t>& a, const std::pair<std::string, uint32_t>& b) {
                       return a.first < b.first;
                     });
  } else {
    t.warnings_.push_back({"readable_grc.tsv", 0, "file missing: Greek glosses come from the lexicon only"});
  }
  return Result<GreekTables>(std::move(t));
}

const ReadableRow* GreekTables::readable(std::string_view key, std::string_view pos) const {
  auto it = std::lower_bound(readable_.begin(), readable_.end(), key,
                             [](const ReadableRow& r, std::string_view k) { return r.key < k; });
  const ReadableRow* first = nullptr;
  for (; it != readable_.end() && it->key == key; ++it) {
    if (pos.empty() || it->pos == pos) return &*it;
    if (!first) first = &*it;
  }
  return pos.empty() ? first : nullptr;
}

const LexRow* GreekTables::find(std::string_view kind, std::string_view source, std::string_view frame) const {
  for (const LexRow& r : rows_)
    if (r.kind == kind && r.source == source && (frame.empty() || r.frame == frame)) return &r;
  return nullptr;
}

bool GreekTables::durative(std::string_view sourceLemma) const { return find("durative", sourceLemma) != nullptr; }

void GreekTables::taught(std::string_view english, std::vector<const TaughtGloss*>& out) const {
  const std::string k = text::lower(english);
  auto it = std::lower_bound(taughtIndex_.begin(), taughtIndex_.end(), k,
                             [](const std::pair<std::string, uint32_t>& e, const std::string& x) { return e.first < x; });
  for (; it != taughtIndex_.end() && it->first == k; ++it) {
    const TaughtGloss* g = &taught_[it->second];
    if (std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g);
  }
}

void GreekTables::taughtReadable(std::string_view word, bool es, std::vector<const TaughtGloss*>& out) const {
  const std::string k = std::string(es ? "es:" : "en:") + text::nfc(text::lower(word));
  auto it = std::lower_bound(readableIndex_.begin(), readableIndex_.end(), k,
                             [](const std::pair<std::string, uint32_t>& e, const std::string& x) { return e.first < x; });
  for (; it != readableIndex_.end() && it->first == k; ++it) {
    const TaughtGloss* g = &readableTaught_[it->second];
    bool dup = false;
    for (const TaughtGloss* o : out) dup = dup || (o->key == g->key && o->pos == g->pos);
    if (!dup) out.push_back(g);
  }
}

// ---- cue helpers ----------------------------------------------------------------------------------------------------
const subs::BreakHints& greekBreakHints() {
  static const subs::BreakHints h = [] {
    subs::BreakHints x;
    for (const char* w : {"καί", "καὶ", "ἀλλά", "ἀλλὰ", "ὅτι", "εἰ", "ἐάν", "ἐὰν", "ἐπεί", "ἐπεὶ", "ὅτε", "ἵνα",
                          "ὥστε", "ἐν", "εἰς", "ἐκ", "ἐξ", "πρός", "πρὸς", "ἀπό", "ἀπὸ", "μετά", "μετὰ", "διά",
                          "διὰ", "περί", "περὶ", "ὑπό", "ὑπὸ", "παρά", "παρὰ", "ἐπί", "ἐπὶ", "ἤ", "οὐδέ", "οὐδὲ",
                          "Καί", "Καὶ", "Ἀλλά", "Ἀλλὰ"})
      x.breakBefore.emplace_back(text::nfc(w));
    return x;
  }();
  return h;
}

std::string capitaliseGreek(std::string_view s) {
  std::string t(s);
  size_t i = 0;
  bool inTag = false;
  while (i < t.size()) {
    size_t j = i;
    const char32_t c = text::decodeUtf8(t, j);
    if (inTag) { if (c == '>' || c == '}') inTag = false; i = j; continue; }
    if (c == '<' || c == '{') { inTag = true; i = j; continue; }
    if (c == ' ' || c == '"' || c == '\'' || c == '(' || c == '[' || c == 0xBF || c == 0xA1 || c == 0x2018 ||
        c == 0x201C || c == 0xAB || c == '-' || c == 0x2014 || c == 0x2013 || c == 0x266A) {
      i = j;
      continue;
    }
    // decompose the first letter with its marks, upper-case the base, recompose
    size_t k = j;
    while (k < t.size()) {
      size_t m = k;
      const char32_t d = text::decodeUtf8(t, m);
      if (d < 0x300 || d > 0x36F) break;
      k = m;
    }
    std::string letter = text::nfd(t.substr(i, k - i));
    size_t p = 0;
    const char32_t base = text::decodeUtf8(letter, p);
    char32_t up = base;
    if (base >= 0x03B1 && base <= 0x03C9 && base != 0x03C2) up = base - 0x20;
    else if (base >= 'a' && base <= 'z') up = base - 32;
    else if (base >= 0xE0 && base <= 0xFE && base != 0xF7) up = base - 32;
    if (up == base) return t;
    std::string rebuilt;
    text::appendUtf8(rebuilt, up);
    rebuilt += letter.substr(p);
    t.replace(i, k - i, text::nfc(rebuilt));
    return t;
  }
  return t;
}

namespace {
// One word: every accent -> acute (one), breathings / iota subscript / length marks dropped, diaeresis kept.
std::string monoWord(std::string_view w) {
  const std::string d = text::nfd(w);
  std::string out;
  size_t i = 0;
  bool acute = false;
  while (i < d.size()) {
    const char32_t c = text::decodeUtf8(d, i);
    switch (c) {
      case 0x301: case 0x300: case 0x342: case 0x341: case 0x340:
        if (!acute) text::appendUtf8(out, 0x301);
        acute = true;
        break;
      case 0x313: case 0x314: case 0x345: case 0x304: case 0x306: case 0x343: break;
      default: text::appendUtf8(out, c); break;
    }
  }
  return text::nfc(out);
}
bool greekLetterOrMark(char32_t c) {
  return (c >= 0x0370 && c <= 0x03FF && c != 0x037E && c != 0x0387) || (c >= 0x1F00 && c <= 0x1FFF && c != 0x1FBD &&
                                                                          c != 0x1FBF && c != 0x1FFD && c != 0x1FFE) ||
         (c >= 0x300 && c <= 0x36F);
}
}  // namespace

std::string toMonotonic(std::string_view s) {
  // Monosyllables lose their accent (standard monotonic), except ἤ "or" and the interrogatives ποῦ ποῖ πῶς πῇ τίς τί.
  static const char* const kKeep[] = {"ἤ", "ποῦ", "ποῖ", "πῶς", "πῇ", "τίς", "τί"};
  const std::string t = text::nfc(s);
  std::string out;
  size_t i = 0;
  while (i < t.size()) {
    size_t j = i;
    const char32_t c = text::decodeUtf8(t, j);
    if (!greekLetterOrMark(c)) { out += t.substr(i, j - i); i = j; continue; }
    size_t k = j;
    while (k < t.size()) {
      size_t m = k;
      if (!greekLetterOrMark(text::decodeUtf8(t, m))) break;
      k = m;
    }
    const std::string word = t.substr(i, k - i);
    std::string mono = monoWord(word);
    if (accentOf(word).syllables == 1) {
      bool keep = false;
      const std::string key = text::greek_key(ultimaToAcute(text::lower(word)));
      for (const char* x : kKeep) keep = keep || key == text::greek_key(x);
      if (!keep) mono = text::nfc(stripAccents(mono));
    }
    out += mono;
    i = k;
  }
  return out;
}

}  // namespace vp::grc
