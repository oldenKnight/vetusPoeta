// Readable tables (readable_en.tsv / readable_es.tsv), gloss_es_la.tsv rows and small string helpers.
#include <algorithm>
#include <fstream>

#include "internal.h"
#include "vp/text.h"

namespace vp::la2x::detail {

namespace {
std::vector<std::string> splitTabs(const std::string& line) {
  std::vector<std::string> c;
  size_t a = 0;
  for (;;) {
    const size_t b = line.find('\t', a);
    c.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  for (std::string& x : c) {
    while (!x.empty() && (x.back() == ' ' || x.back() == '\r')) x.pop_back();
    size_t k = 0;
    while (k < x.size() && x[k] == ' ') ++k;
    x.erase(0, k);
  }
  return c;
}
bool rowLess(const Row& a, const Row& b) {
  if (a.kind != b.kind) return a.kind < b.kind;
  if (a.latin != b.latin) return a.latin < b.latin;
  return a.feature < b.feature;
}
}  // namespace

bool noteHas(std::string_view note, std::string_view tag) {
  size_t a = 0;
  while (a < note.size()) {
    while (a < note.size() && (note[a] == ' ' || note[a] == ',' || note[a] == ';')) ++a;
    size_t b = a;
    while (b < note.size() && note[b] != ' ' && note[b] != ',' && note[b] != ';') ++b;
    if (b > a && note.substr(a, b - a) == tag) return true;
    a = b;
  }
  return false;
}

bool Tables::load(const std::filesystem::path& file, std::string& error) {
  rows_.clear();
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    error = "cannot read " + file.string();
    return false;
  }
  std::string line;
  std::vector<Row> rows;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> c = splitTabs(line);
    if (c.size() < 4 || c[0].empty() || c[1].empty()) continue;
    Row r;
    r.kind = c[0];
    r.latin = text::nfc(c[1]);
    // the latin column is a key (macrons folded) except for person codes ("1sg") and phrases (several words)
    if (r.kind != "pron" && r.kind != "article") {
      std::string k;
      for (size_t i = 0; i < r.latin.size();) {
        size_t j = r.latin.find(' ', i);
        if (j == std::string::npos) j = r.latin.size();
        if (!k.empty()) k += ' ';
        k += text::latin_key(r.latin.substr(i, j - i));
        i = j + 1;
      }
      r.latin = k;
    }
    r.feature = c[2].empty() ? "-" : c[2];
    r.text = text::nfc(c[3]);
    if (r.text == "-") r.text.clear();
    r.note = c.size() > 4 ? c[4] : std::string();
    rows.push_back(std::move(r));
  }
  std::stable_sort(rows.begin(), rows.end(), rowLess);
  for (Row& r : rows)
    if (rows_.empty() || rowLess(rows_.back(), r)) rows_.push_back(std::move(r));
  return true;
}

const Row* Tables::find(std::string_view kind, std::string_view latin, std::string_view feature) const {
  Row k;
  k.kind = std::string(kind);
  k.latin = std::string(latin);
  k.feature = std::string(feature);
  auto it = std::lower_bound(rows_.begin(), rows_.end(), k, rowLess);
  if (it != rows_.end() && it->kind == kind && it->latin == latin && it->feature == feature) return &*it;
  return nullptr;
}

const Row* Tables::best(std::string_view kind, std::string_view latin,
                        std::initializer_list<std::string_view> features) const {
  for (std::string_view f : features)
    if (const Row* r = find(kind, latin, f)) return r;
  return find(kind, latin, "-");
}

bool Tables::has(std::string_view kind, std::string_view latin) const {
  Row k;
  k.kind = std::string(kind);
  k.latin = std::string(latin);
  auto it = std::lower_bound(rows_.begin(), rows_.end(), k, rowLess);
  return it != rows_.end() && it->kind == kind && it->latin == latin;
}

std::string Tables::text(std::string_view kind, std::string_view latin,
                         std::initializer_list<std::string_view> features) const {
  const Row* r = best(kind, latin, features);
  return r ? r->text : std::string();
}

bool Tables::tagged(std::string_view kind, std::string_view latin, std::string_view tag) const {
  Row k;
  k.kind = std::string(kind);
  k.latin = std::string(latin);
  for (auto it = std::lower_bound(rows_.begin(), rows_.end(), k, rowLess);
       it != rows_.end() && it->kind == kind && it->latin == latin; ++it)
    if (noteHas(it->note, tag)) return true;
  return false;
}

bool loadGlossEs(const std::filesystem::path& file, std::vector<GlossEsRow>& out) {
  out.clear();
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> c = splitTabs(line);
    if (c.size() < 3 || c[0].empty() || c[2].empty()) continue;
    out.push_back(GlossEsRow{text::latin_key(c[0]), text::nfc(c[1]), text::nfc(c[2])});
  }
  return true;
}

bool Discourse::seen(uint32_t l) const {
  return l != lex::kNoLemma && std::find(mentioned.begin(), mentioned.end(), l) != mentioned.end();
}
void Discourse::mention(uint32_t l) {
  if (l == lex::kNoLemma || seen(l)) return;
  if (mentioned.size() >= 64) mentioned.erase(mentioned.begin());
  mentioned.push_back(l);
}

void appendWord(std::string& out, std::string_view w) {
  if (w.empty()) return;
  const bool opener = out.size() >= 2 && (unsigned char)out[out.size() - 2] == 0xC2 &&
                      ((unsigned char)out.back() == 0xBF || (unsigned char)out.back() == 0xA1);   // ¿ ¡
  if (!out.empty() && out.back() != ' ' && out.back() != '(' && !opener && w[0] != ',') out += ' ';
  out.append(w.data(), w.size());
}

std::string capitaliseFirst(std::string_view s) {
  std::string out(s);
  size_t i = 0;
  // skip opening marks (¿ ¡ quotes)
  while (i < out.size()) {
    size_t j = i;
    const char32_t c = text::decodeUtf8(out, j);
    if (c == U'¿' || c == U'¡' || c == U'"' || c == U'“' || c == U'(') { i = j; continue; }
    std::string one;
    text::appendUtf8(one, c);
    std::string up;
    if (c >= U'a' && c <= U'z') up = std::string(1, (char)(c - 32));
    else if (c >= 0xE0 && c <= 0xFE && c != 0xF7) text::appendUtf8(up, c - 32);
    else up = one;
    out.replace(i, j - i, up);
    break;
  }
  return out;
}

std::string stripMacrons(std::string_view s) { return text::display_latin(s, false); }

}  // namespace vp::la2x::detail
