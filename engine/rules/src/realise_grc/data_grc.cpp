// GreekData: loaders for the Greek curated tables (vp/realise_grc.h). Same file conventions as curated.cpp: UTF-8,
// tab-separated, '#' comments and blank lines ignored, a missing file is an error, a bad line a warning.
#include <algorithm>

#include "vp/fs.h"
#include "vp/realise_grc.h"
#include "vp/text.h"

namespace vp::grc {

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\r' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

void splitTabs(std::string_view line, std::vector<std::string>& cols) {
  cols.clear();
  size_t start = 0;
  for (;;) {
    size_t t = line.find('\t', start);
    if (t == std::string_view::npos) { cols.emplace_back(trim(line.substr(start))); break; }
    cols.emplace_back(trim(line.substr(start, t - start)));
    start = t + 1;
  }
}

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

std::string col(const std::vector<std::string>& c, size_t i) { return i < c.size() ? c[i] : std::string(); }

uint8_t caseOf(std::string_view s) {
  if (s == "nom") return feat::Nom;
  if (s == "gen") return feat::Gen;
  if (s == "dat") return feat::Dat;
  if (s == "acc") return feat::Acc;
  if (s == "voc") return feat::Voc;
  return 0;
}

Frame parseGreekFrame(std::string_view s) {
  Frame f;
  f.raw = std::string(s);
  if (s.substr(0, 4) == "mid:") { f.middle = true; s.remove_prefix(4); }
  if (s == "acc") f.kind = FrameKind::Acc;
  else if (s == "gen") f.kind = FrameKind::Gen;
  else if (s == "dat") f.kind = FrameKind::Dat;
  else if (s == "dat+acc") f.kind = FrameKind::DatAcc;
  else if (s == "acc+acc") f.kind = FrameKind::AccAcc;
  else if (s == "acc+inf") f.kind = FrameKind::AccInf;
  else if (s == "inf") f.kind = FrameKind::Inf;
  else if (s == "intr") f.kind = FrameKind::Intr;
  else if (s == "copula") f.kind = FrameKind::Copula;
  else if (s == "impers:acc+inf") f.kind = FrameKind::ImpersAccInf;
  else if (s == "impers:dat+inf") f.kind = FrameKind::ImpersDatInf;
  else if (s == "purp:inf") f.kind = FrameKind::PurpInf;   // C18
  else if (s.substr(0, 5) == "prep:") {
    std::string_view rest = s.substr(5);
    size_t plus = rest.find('+');
    if (plus != std::string_view::npos) {
      f.kind = FrameKind::Prep;
      f.prepKey = text::greek_key(rest.substr(0, plus));
      f.prepCase = caseOf(rest.substr(plus + 1));
    }
  }
  return f;
}

bool isSlotName(std::string_view s) {
  static const char* const k[] = {"VOC", "CONN", "S", "IO", "O", "OBL", "ADV", "NEG", "V", "PRED", "INF", "WH"};
  for (const char* n : k)
    if (s == n) return true;
  return false;
}

void words(std::string_view s, std::vector<std::string>& out) {
  out.clear();
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == ' ') ++i;
    size_t e = s.find(' ', i);
    if (e == std::string_view::npos) e = s.size();
    if (e > i) out.emplace_back(s.substr(i, e - i));
    i = e;
  }
}

template <class T> const T* byKey(const std::vector<T>& v, std::string_view key, std::string T::*field) {
  auto it = std::lower_bound(v.begin(), v.end(), key, [&](const T& a, std::string_view k) { return a.*field < k; });
  if (it != v.end() && (*it).*field == key) return &*it;
  return nullptr;
}

}  // namespace

struct GreekLoader {
  GreekData& d;
  std::string file;
  std::vector<std::string> cols;
  void warn(int line, std::string msg) { d.warnings_.push_back(curated::LoadWarning{file, line, std::move(msg)}); }
  bool need(int line, size_t n) {
    if (cols.size() >= n && !cols[0].empty()) return true;
    warn(line, "expected at least " + std::to_string(n) + " tab-separated columns, got " + std::to_string(cols.size()));
    return false;
  }

  void valency(std::string_view data) {
    forLines(data, [&](int no, std::string_view line) {
      splitTabs(line, cols);
      if (!need(no, 2)) return;
      Valency v;
      v.key = text::greek_key(cols[0]);
      v.example = col(cols, 2);
      v.note = col(cols, 3);
      std::string_view fr = cols[1];
      size_t p = 0;
      while (p <= fr.size()) {
        size_t semi = fr.find(';', p);
        std::string_view one = trim(fr.substr(p, semi == std::string_view::npos ? std::string_view::npos : semi - p));
        if (!one.empty()) {
          Frame f = parseGreekFrame(one);
          if (f.kind == FrameKind::Other) warn(no, "unknown frame '" + std::string(one) + "'");
          v.frames.push_back(std::move(f));
        }
        if (semi == std::string_view::npos) break;
        p = semi + 1;
      }
      d.valency_.push_back(std::move(v));
    });
    std::stable_sort(d.valency_.begin(), d.valency_.end(), [](const Valency& a, const Valency& b) { return a.key < b.key; });
    for (size_t i = 1; i < d.valency_.size(); ++i)
      if (d.valency_[i].key == d.valency_[i - 1].key) warn(0, "duplicate valency key '" + d.valency_[i].key + "'");
  }

  void preps(std::string_view data) {
    forLines(data, [&](int no, std::string_view line) {
      splitTabs(line, cols);
      if (!need(no, 4)) return;
      PrepEntry e;
      e.english = cols[0];
      e.context = cols[1];
      e.greek = cols[2];
      e.caseRaw = cols[3];
      e.note = col(cols, 4);
      if (e.greek != "-") e.greekKey = text::greek_key(e.greek);
      e.infinitive = e.caseRaw == "inf";
      e.case_ = caseOf(e.caseRaw);
      if (!e.case_ && !e.infinitive && e.caseRaw != "-") warn(no, "unknown case '" + e.caseRaw + "'");
      if (!e.greekKey.empty() && e.case_) {
        auto it = std::find_if(d.prepCases_.begin(), d.prepCases_.end(),
                               [&](const std::pair<std::string, uint16_t>& p) { return p.first == e.greekKey; });
        if (it == d.prepCases_.end()) d.prepCases_.push_back({e.greekKey, (uint16_t)(1u << e.case_)});
        else it->second |= (uint16_t)(1u << e.case_);
      }
      d.preps_.push_back(std::move(e));
    });
    std::sort(d.prepCases_.begin(), d.prepCases_.end());
  }

  void names(std::string_view data) {
    forLines(data, [&](int no, std::string_view line) {
      splitTabs(line, cols);
      if (!need(no, 6)) return;
      NameEntry n;
      n.english = cols[0];
      n.nom = text::nfc(cols[1]);
      n.gen = text::nfc(cols[2]);
      n.gender = cols[3] == "m" ? feat::M : cols[3] == "f" ? feat::F : cols[3] == "n" ? feat::N : 0;
      if (cols[4] == "-") n.declension = -1;
      else if (cols[4].size() == 1 && cols[4][0] >= '0' && cols[4][0] <= '3') n.declension = cols[4][0] - '0';
      else warn(no, "declension must be 0..3 or -");
      if (cols[5] == "keep") n.policy = curated::NamePolicy::Keep;
      else if (cols[5] == "decline") n.policy = curated::NamePolicy::Decline;
      else if (cols[5] == "translate") n.policy = curated::NamePolicy::Translate;
      else warn(no, "policy must be keep / decline / translate");
      n.note = col(cols, 6);
      auto field = [&](const char* key) {   // "key=value" up to the next space
        const std::string k = std::string(key) + "=";
        for (size_t v = n.note.find(k); v != std::string::npos; v = n.note.find(k, v + 1)) {
          if (v > 0 && n.note[v - 1] != ' ') continue;
          const size_t e = n.note.find(' ', v);
          return n.note.substr(v + k.size(), e == std::string::npos ? std::string::npos : e - v - k.size());
        }
        return std::string();
      };
      auto word = [&](const char* w) {   // a bare word of the note
        const std::string x(w);
        for (size_t v = n.note.find(x); v != std::string::npos; v = n.note.find(x, v + 1)) {
          const size_t e = v + x.size();
          if ((v == 0 || n.note[v - 1] == ' ') && (e == n.note.size() || n.note[e] == ' ')) return true;
        }
        return false;
      };
      n.voc = text::nfc(field("voc"));
      // C29: spelled-out forms, plural names, places, Spanish spellings, neologisms
      if (const std::string fs = field("forms"); !fs.empty()) {
        size_t a = 0;
        while (a <= fs.size()) {
          const size_t c = fs.find(',', a);
          n.forms.push_back(text::nfc(fs.substr(a, c == std::string::npos ? std::string::npos : c - a)));
          if (c == std::string::npos) break;
          a = c + 1;
        }
        if (n.forms.size() < 4) { warn(no, "forms= needs nominative, genitive, dative, accusative (and a vocative)"); n.forms.clear(); }
      }
      if (word("pl")) n.number = feat::Pl;
      n.place = word("place");
      n.article = word("art");
      n.neologism = word("neologism");
      n.spanish = field("es");
      d.names_.push_back(std::move(n));
    });
  }

  void particles(std::string_view data) {
    forLines(data, [&](int no, std::string_view line) {
      splitTabs(line, cols);
      if (!need(no, 2)) return;
      ParticleEntry p;
      p.head = text::nfc(cols[0]);
      p.key = text::greek_key(cols[0]);
      if (cols[1] == "second") p.second = true;
      else if (cols[1] != "first") warn(no, "position must be first / second");
      p.function = col(cols, 2);
      p.english = col(cols, 3);
      p.note = col(cols, 4);
      d.particles_.push_back(std::move(p));
    });
    std::stable_sort(d.particles_.begin(), d.particles_.end(),
                     [](const ParticleEntry& a, const ParticleEntry& b) { return a.key < b.key; });
  }

  void phrasebook(std::string_view data) {
    forLines(data, [&](int no, std::string_view line) {
      splitTabs(line, cols);
      if (!need(no, 2)) return;
      curated::PhraseEntry e;
      e.pattern = cols[0];
      e.latin = text::nfc(cols[1]);   // the target-language column (Greek here)
      const std::string t = col(cols, 2);
      e.tier = t == "1" ? 1 : t == "2" ? 2 : t == "3" ? 3 : 0;
      e.reg = col(cols, 3);
      e.note = col(cols, 4);
      d.phrasebook_.push_back(std::move(e));
    });
  }

  void order(std::string_view data) {
    forLines(data, [&](int no, std::string_view line) {
      std::string_view t = trim(line);
      if (t.substr(0, 5) != "RULE ") { warn(no, "expected 'RULE <id> : <condition> => <ordering> ; <note>'"); return; }
      t.remove_prefix(5);
      const size_t colon = t.find(" : ");
      const size_t arrow = t.find("=>");
      if (colon == std::string_view::npos || arrow == std::string_view::npos || arrow < colon) {
        warn(no, "rule without ' : ' or '=>'");
        return;
      }
      curated::OrderRule r;
      r.line = no;
      r.id = std::string(trim(t.substr(0, colon)));
      r.conditionText = std::string(trim(t.substr(colon + 3, arrow - colon - 3)));
      std::string_view rest = t.substr(arrow + 2);
      const size_t semi = rest.find(" ; ");
      r.orderingText = std::string(trim(rest.substr(0, semi)));
      if (semi != std::string_view::npos) r.note = std::string(trim(rest.substr(semi + 3)));
      words(r.conditionText, r.condition);
      words(r.orderingText, r.ordering);
      d.order_.push_back(std::move(r));
    });
  }
};

Result<GreekData> GreekData::load(const std::filesystem::path& dir) {
  GreekData d;
  GreekLoader L{d, {}, {}};
  struct F { const char* name; void (GreekLoader::*fn)(std::string_view); };
  static const F files[] = {{"valency_grc.tsv", &GreekLoader::valency},   {"preps_en_grc.tsv", &GreekLoader::preps},
                            {"names_grc.tsv", &GreekLoader::names},       {"particles_grc.tsv", &GreekLoader::particles},
                            {"phrasebook_en_grc.tsv", &GreekLoader::phrasebook}, {"order_grc.txt", &GreekLoader::order}};
  for (const F& f : files) {
    auto r = fs::readFile((dir / f.name).string());
    if (!r.ok())
      return Result<GreekData>(ErrorCode::NotFound, std::string("curated file missing: ") + f.name,
                               "Reinstall vetus poeta or restore data/curated/" + std::string(f.name) + ".");
    L.file = f.name;
    (L.*(f.fn))(r.value());
  }
  return d;
}

const Valency* GreekData::valency(std::string_view key) const { return byKey(valency_, key, &Valency::key); }

uint16_t GreekData::prepCases(std::string_view key) const {
  auto it = std::lower_bound(prepCases_.begin(), prepCases_.end(), key,
                             [](const std::pair<std::string, uint16_t>& p, std::string_view k) { return p.first < k; });
  return it != prepCases_.end() && it->first == key ? it->second : 0;
}

uint8_t GreekData::prepDefaultCase(std::string_view key) const {
  for (const PrepEntry& p : preps_)
    if (p.greekKey == key && p.case_) return p.case_;
  return 0;
}

const NameEntry* GreekData::nameByEnglish(std::string_view english) const {
  const std::string k = text::en_key(english);
  for (const NameEntry& n : names_)
    if (text::en_key(n.english) == k) return &n;
  // C29: the Spanish spelling of a place ("Atenas", "México"), accents ignored
  const std::string b = text::es_bare(english);
  for (const NameEntry& n : names_)
    if (!n.spanish.empty() && text::es_bare(n.spanish) == b) return &n;
  return nullptr;
}

const NameEntry* GreekData::nameByGreek(std::string_view key) const {
  for (const NameEntry& n : names_)
    if (text::greek_key(n.nom) == key) return &n;
  return nullptr;
}

const ParticleEntry* GreekData::particle(std::string_view key) const { return byKey(particles_, key, &ParticleEntry::key); }

const curated::OrderRule* GreekData::rule(std::string_view id) const {
  for (const curated::OrderRule& r : order_)
    if (r.id == id) return &r;
  return nullptr;
}

std::vector<std::string> GreekData::conditionSet(std::string_view id) const {
  std::vector<std::string> out;
  const curated::OrderRule* r = rule(id);
  if (!r) return out;
  const size_t a = r->conditionText.find('{'), b = r->conditionText.find('}');
  if (a == std::string::npos || b == std::string::npos || b < a) return out;
  std::string inner = r->conditionText.substr(a + 1, b - a - 1);
  std::replace(inner.begin(), inner.end(), ',', ' ');
  words(inner, out);
  for (std::string& w : out) w = text::greek_key(w);
  return out;
}

std::vector<std::string> GreekData::orderingList(std::string_view id, std::string_view marker) const {
  std::vector<std::string> out;
  const curated::OrderRule* r = rule(id);
  if (!r) return out;
  size_t m = marker.empty() ? 0 : r->orderingText.find(std::string(marker));
  if (m == std::string::npos) return out;
  const size_t a = r->orderingText.find('(', m), b = r->orderingText.find(')', a == std::string::npos ? m : a);
  if (a == std::string::npos || b == std::string::npos) return out;
  std::string inner = r->orderingText.substr(a + 1, b - a - 1);
  std::replace(inner.begin(), inner.end(), ',', ' ');
  words(inner, out);
  for (std::string& w : out) w = text::greek_key(w);
  return out;
}

std::vector<std::string> GreekData::slotTemplate(std::string_view id) const {
  std::vector<std::string> out;
  const curated::OrderRule* r = rule(id);
  if (!r) return out;
  for (const std::string& tok : r->ordering) {
    std::string s = tok;
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '[' || c == ']' || c == ','; }), s.end());
    if (!isSlotName(s)) break;
    out.push_back(s);
  }
  return out;
}

}  // namespace vp::grc
