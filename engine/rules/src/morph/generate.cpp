// generate(), feature builders, findLemma, principal-part parsing and display helpers (vp/morph.h).
#include <algorithm>

#include "paradigm_la.h"
#include "vp/morph.h"
#include "vp/text.h"

namespace vp::morph {

using namespace vp::feat;

// ---- feature builders --------------------------------------------------------------------------------------------
Features nounForm(uint8_t case_, uint8_t number) {
  Features f; f.pos = Noun; f.case_ = case_; f.number = number; return f;
}
Features adjForm(uint8_t case_, uint8_t number, uint8_t gender, uint8_t degree) {
  Features f; f.pos = Adj; f.case_ = case_; f.number = number; f.gender = gender; f.degree = degree; return f;
}
Features verbForm(uint8_t person, uint8_t number, uint8_t tense, uint8_t mood, uint8_t voice) {
  Features f; f.pos = Verb; f.person = person; f.number = number; f.tense = tense; f.mood = mood; f.voice = voice;
  return f;
}
Features participle(uint8_t tense, uint8_t voice, uint8_t case_, uint8_t number, uint8_t gender) {
  Features f; f.pos = Verb; f.mood = ParticipleMood; f.tense = tense; f.voice = voice; f.case_ = case_;
  f.number = number; f.gender = gender; return f;
}
Features infinitive(uint8_t tense, uint8_t voice) {
  Features f; f.pos = Verb; f.mood = Infinitive; f.tense = tense; f.voice = voice; return f;
}
Features imperative(uint8_t number, uint8_t voice) {
  Features f; f.pos = Verb; f.mood = Imperative; f.tense = Present; f.person = P2; f.number = number; f.voice = voice;
  return f;
}

bool genderAdmits(uint8_t have, uint8_t want) {
  if (have == want) return true;
  switch (want) {
    case M: return have == MF || have == MN || have == MFN;
    case F: return have == MF || have == FN || have == MFN;
    case N: return have == MN || have == FN || have == MFN;
    default: return false;
  }
}

uint32_t packedOf(const lex::Lexicon& lx, const lex::Analysis& a) { return lx.feature(a.feat); }

// ---- display -----------------------------------------------------------------------------------------------------
// C15: some library headwords carry editorial marks ("((caelum", "((alius"): a word never keeps brackets or
// punctuation at its edges.
std::string cleanHead(std::string_view w) {
  auto mark = [](char c) {
    return c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '*' || c == '?' || c == '!' ||
           c == ',' || c == ';' || c == ':' || c == '"' || c == '<' || c == '>' || c == '|' || c == '.';
  };
  if (w.size() > 2 && w.front() == '[' && w.back() == ']') return std::string(w);   // an unknown word "[x]" stays marked
  size_t a = 0, b = w.size();
  while (a < b && mark(w[a])) ++a;
  while (b > a && mark(w[b - 1])) --b;
  return std::string(w.substr(a, b - a));
}

std::string displayForm(std::string_view form0, bool macrons) {
  const std::string form = cleanHead(form0);
  std::string d = text::nfd(form);
  // Anceps vowels carry both U+0304 and U+0306 ("egō̆", "mihī̆"): show them plain.
  std::u32string u = text::toUtf32(d), o;
  o.reserve(u.size());
  for (size_t i = 0; i < u.size(); ++i) {
    char32_t c = u[i];
    if (c >= 0x035C && c <= 0x0362) continue;   // tie bars / double breves ("de͡inde" -> "deinde")
    if (c == 0x0304 || c == 0x0306) {
      size_t j = i;
      bool mac = false, brv = false;
      while (j < u.size() && (u[j] == 0x0304 || u[j] == 0x0306)) { mac |= u[j] == 0x0304; brv |= u[j] == 0x0306; ++j; }
      if (mac && brv) { i = j - 1; continue; }
    }
    o.push_back(c);
  }
  return text::display_latin(text::nfc(text::toUtf8(o)), macrons);
}

// ---- principal parts ---------------------------------------------------------------------------------------------
namespace {
std::string firstWord(std::string_view s) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  size_t e = s.find_first_of(" ,;)");
  std::string w(s.substr(0, e));
  while (!w.empty() && w.front() == '*') w.erase(0, 1);
  return w;
}
}  // namespace

Principal parsePrincipal(std::string_view head, std::string_view principal) {
  Principal p;
  p.first = std::string(head);
  const size_t open = principal.find('(');
  if (open != std::string_view::npos) {
    size_t close = principal.find(')', open);
    std::string_view inner = principal.substr(open + 1, close == std::string_view::npos ? std::string_view::npos
                                                                                       : close - open - 1);
    struct Label { const char* text; std::string Principal::*field; };
    static const Label labels[] = {
        {"present infinitive ", &Principal::infinitive}, {"perfect active ", &Principal::perfect},
        {"supine ", &Principal::supine},                 {"genitive ", &Principal::genitive},
        {"feminine ", &Principal::feminine},             {"neuter ", &Principal::neuter},
        {"comparative ", &Principal::comparative},       {"superlative ", &Principal::superlative},
        {"oblique ", &Principal::genitive}};
    size_t pos = 0;
    while (pos < inner.size()) {
      size_t comma = inner.find(", ", pos);
      std::string_view piece = inner.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
      while (!piece.empty() && piece.front() == ' ') piece.remove_prefix(1);
      for (const Label& l : labels) {
        std::string_view lt(l.text);
        if (piece.substr(0, lt.size()) == lt) {
          std::string& dst = p.*(l.field);
          if (dst.empty()) dst = text::nfc(firstWord(piece.substr(lt.size())));
          break;
        }
      }
      if (comma == std::string_view::npos) break;
      pos = comma + 2;
    }
    return p;
  }
  // Short dictionary form: "amō, amāre, amāvī, amātum" / "bonus, bona, bonum" / "puellae, f."
  std::vector<std::string> parts;
  size_t pos = 0;
  while (pos <= principal.size()) {
    size_t comma = principal.find(',', pos);
    std::string w = firstWord(principal.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos));
    parts.push_back(text::nfc(w));
    if (comma == std::string_view::npos) break;
    pos = comma + 1;
  }
  if (parts.empty()) return p;
  const std::string h = text::nfc(head);
  if (parts[0] == h) {
    if (parts.size() >= 3 && detail::endsWith(parts[1], "a") && detail::endsWith(parts[2], "um")) {
      p.feminine = parts[1];
      p.neuter = parts[2];
    } else {
      if (parts.size() > 1) p.infinitive = parts[1];
      if (parts.size() > 2) p.perfect = parts[2];
      if (parts.size() > 3) p.supine = parts[3];
    }
  } else {
    p.genitive = parts[0];
  }
  return p;
}

// ---- findLemma ---------------------------------------------------------------------------------------------------
uint32_t findLemma(const lex::Lexicon& lx, std::string_view head, uint8_t pos) {
  const std::string key = text::latin_key(head);
  if (key.empty()) return lex::kNoLemma;
  thread_local std::vector<lex::Analysis> an;
  an.clear();
  lx.lookup(key, an);
  const std::string wantHead = displayForm(head, true);
  uint32_t best = lex::kNoLemma;
  long bestScore = -1;
  for (const lex::Analysis& a : an) {
    const lex::Lemma l = lx.lemma(a.lemma);
    if (l.id == lex::kNoLemma || l.key != key) continue;
    if (pos && l.pos != pos) continue;
    long s = 0;
    if (displayForm(l.head, true) == wantHead) s += 1 << 20;
    if (l.flags & lex::HasTable) s += 1 << 19;
    s += (l.tier ? (4 - l.tier) : 0) << 16;
    s += 65535 - (l.freqRank ? l.freqRank : 65535);
    if (s > bestScore || (s == bestScore && a.lemma < best)) { bestScore = s; best = a.lemma; }
  }
  return best;
}

// ---- generate ----------------------------------------------------------------------------------------------------
namespace {

bool usable(std::string_view form) { return !form.empty() && form.find(" + ") == std::string_view::npos; }

// Score of a cell for the wanted features; < 0 = not admissible.
int cellScore(const Features& c, const Features& w) {
  if (c.case_ != w.case_ || c.person != w.person || c.tense != w.tense || c.mood != w.mood) return -1;
  int s = 0;
  if (c.number != w.number) {
    if (c.number != 0 || c.case_ == 0) return -1;   // number-less cells of indeclinables (nihil, suī) fit any number
    s -= 1;
  }
  if (w.voice == 0) s += c.voice == Active || c.voice == 0 ? 1 : 0;
  else if (c.voice != w.voice) return -1;
  if (c.gender == w.gender) s += 4;
  else if (w.gender == 0 || c.gender == 0) s += 1;
  else if (genderAdmits(c.gender, w.gender)) s += 2;
  else return -1;
  const bool cPos = c.degree <= Positive, wPos = w.degree <= Positive;
  if (c.degree != w.degree && !(cPos && wPos)) return -1;
  const uint8_t hard = Supine | Gerundive;
  if ((c.extra & hard) != (w.extra & hard)) return -1;
  if (c.extra == w.extra) s += 8;
  else if ((c.extra & Alternative) && !(w.extra & Alternative)) s += 0;
  else s += 4;
  return s;
}

bool bestCell(const lex::Lexicon& lx, uint32_t lemma, const Features& want, std::string_view& form, uint32_t& packed) {
  thread_local std::vector<std::pair<uint32_t, std::string_view>> cells;
  cells.clear();
  lx.cells(lemma, cells);
  int best = -1;
  for (const auto& c : cells) {
    if (!usable(c.second)) continue;
    const int s = cellScore(unpack(c.first), want);
    if (s > best) { best = s; form = c.second; packed = c.first; }
  }
  return best >= 0;
}

bool fromParadigm(const lex::Lemma& l, const Features& want, std::string& form, uint32_t& packed) {
  thread_local std::vector<detail::Cell> cells;
  cells.clear();
  detail::paradigm(l, cells);
  int best = -1;
  for (const detail::Cell& c : cells) {
    const int s = cellScore(unpack(c.packed), want);
    if (s > best) { best = s; form = c.form; packed = c.packed; }
  }
  return best >= 0;
}

// Declines an -us participle / adjective (nominative given) through its own lemma or the 1st/2nd paradigm.
bool declineAdjLike(const lex::Lexicon& lx, std::string_view nomM, const Features& want, std::string& out,
                    bool macrons, GenInfo* info) {
  uint32_t l2 = findLemma(lx, nomM, Participle);
  if (l2 == lex::kNoLemma) l2 = findLemma(lx, nomM, Adj);
  Features w = want;
  w.pos = 0; w.mood = 0; w.tense = 0; w.voice = 0; w.person = 0;
  if (l2 != lex::kNoLemma) {
    std::string_view f; uint32_t pk = 0;
    if (bestCell(lx, l2, w, f, pk)) {
      out = displayForm(f, macrons);
      if (info) { info->lemmaUsed = l2; info->packed = pk; }
      return true;
    }
  }
  if (!detail::endsWith(nomM, "us")) {
    if (want.case_ == Nom && want.number == Sg) { out = displayForm(nomM, macrons); return true; }
    return false;
  }
  thread_local std::vector<detail::Cell> cells;
  cells.clear();
  detail::adjective12(nomM, detail::dropSuffix(nomM, "us"), Participle, 0, cells);
  for (const detail::Cell& c : cells)
    if (cellScore(unpack(c.packed), w) >= 0) {
      out = displayForm(c.form, macrons);
      if (info) { info->fromRule = true; info->packed = c.packed; }
      return true;
    }
  return false;
}

// The nominative perfect participle of a verb ("amātus", "secūtus"), from its cells or the paradigm.
bool perfectParticiple(const lex::Lexicon& lx, const lex::Lemma& l, std::string& nom, bool& fromRule) {
  const bool dep = (l.flags & lex::Deponent) != 0;
  Features w; w.mood = ParticipleMood; w.tense = Perfect; w.voice = dep ? Active : Passive;
  std::string_view f; uint32_t pk = 0;
  if (bestCell(lx, l.id, w, f, pk)) { nom = std::string(f); return true; }
  if (dep) {   // deponent "perfect indicative active" cell without person holds the participle
    Features w2; w2.mood = Indicative; w2.tense = Perfect; w2.voice = Active;
    if (bestCell(lx, l.id, w2, f, pk) && f.find(' ') == std::string_view::npos) { nom = std::string(f); return true; }
  }
  if (detail::paradigmApplies(l) && fromParadigm(l, w, nom, pk)) { fromRule = true; return true; }
  return false;
}

}  // namespace

bool generate(const lex::Lexicon& lx, uint32_t lemma, const Features& f, std::string& out, bool macrons) {
  return generate(lx, lemma, f, out, macrons, nullptr);
}

bool generate(const lex::Lexicon& lx, uint32_t lemma, const Features& f, std::string& out, bool macrons,
              GenInfo* info) {
  if (info) *info = GenInfo{};
  const lex::Lemma l = lx.lemma(lemma);
  if (l.id == lex::kNoLemma) return false;
  if (info) info->lemmaUsed = lemma;
  Features want = f;
  const bool dep = (l.flags & lex::Deponent) != 0;
  if (dep && want.voice == Passive) want.voice = Active;   // deponent cells are tagged active

  // Comparison of adjectives: own cells, then the comparative/superlative lemma named in the principal line,
  // then the regular -ior / -issimus paradigm.
  if ((want.degree == Comparative || want.degree == Superlative) && (l.pos == Adj || l.pos == Participle)) {
    std::string_view cf; uint32_t pk = 0;
    Features w = want;
    if (bestCell(lx, lemma, w, cf, pk) && unpack(pk).degree == want.degree) {
      out = displayForm(cf, macrons);
      if (info) { info->exact = true; info->packed = pk; }
      return true;
    }
    const Principal p = parsePrincipal(l.head, l.principal);
    const std::string& other = want.degree == Comparative ? p.comparative : p.superlative;
    if (!other.empty() && other != "magis") {
      const uint32_t l2 = findLemma(lx, other, Adj);
      if (l2 != lex::kNoLemma && l2 != lemma) {
        w.degree = 0;
        if (bestCell(lx, l2, w, cf, pk)) {
          out = displayForm(cf, macrons);
          if (info) { info->lemmaUsed = l2; info->packed = pk; }
          return true;
        }
      }
    }
    const std::string stem = detail::adjStem(text::nfc(l.head), p.feminine);
    if (stem.empty() || l.cls != 1) return false;
    thread_local std::vector<detail::Cell> cells;
    cells.clear();
    if (want.degree == Comparative) detail::comparative(stem, Adj, cells);
    else {
      const std::string sup = detail::superlativeNom(text::nfc(l.head), stem);
      detail::adjective12(sup, detail::dropSuffix(sup, "us"), Adj, Superlative, cells);
    }
    for (const detail::Cell& c : cells)
      if (cellScore(unpack(c.packed), want) >= 0) {
        out = displayForm(c.form, macrons);
        if (info) { info->fromRule = true; info->packed = c.packed; }
        return true;
      }
    return false;
  }

  // 1. exact FEAT cell
  bool found = false;
  const uint16_t fid = lx.featId(pack(want), found);
  std::string_view form;
  if (found && lx.generate(lemma, lx.feature(fid), form) && usable(form)) {
    out = displayForm(form, macrons);
    if (info) { info->exact = true; info->packed = pack(want); }
    return true;
  }
  // 2. tolerant cell match (merged genders, degree none = positive, alternatives last)
  uint32_t pk = 0;
  const bool periphrasticTense = (want.tense == Perfect || want.tense == Pluperfect || want.tense == FuturePerfect) &&
                                 (want.mood == Indicative || want.mood == Subjunctive) &&
                                 (want.voice == Passive || dep);
  if (!periphrasticTense && bestCell(lx, lemma, want, form, pk)) {
    out = displayForm(form, macrons);
    if (info) info->packed = pk;
    return true;
  }
  if (periphrasticTense && want.person && bestCell(lx, lemma, want, form, pk) && unpack(pk).person) {
    out = displayForm(form, macrons);
    if (info) info->packed = pk;
    return true;
  }
  // 3. participles with a case: decline the participle through its own lemma (or the 1st/2nd paradigm)
  if (want.mood == ParticipleMood && want.case_) {
    Features base = want;
    base.case_ = 0; base.number = 0; base.gender = 0;
    std::string nom;
    bool rule = false;
    if (bestCell(lx, lemma, base, form, pk)) nom = std::string(form);
    else if (detail::paradigmApplies(l) && fromParadigm(l, base, nom, pk)) rule = true;
    if (nom.empty()) return false;
    Features w = want;
    if (!declineAdjLike(lx, nom, w, out, macrons, info)) return false;
    if (info) info->fromRule = info->fromRule || rule;
    return true;
  }
  // 4. periphrastic perfect passive / deponent perfect: participle (nom, agreeing) + sum
  if (periphrasticTense && want.person && want.number) {
    std::string nom;
    bool rule = false;
    if (!perfectParticiple(lx, l, nom, rule)) return false;
    Features pw; pw.case_ = Nom; pw.number = want.number; pw.gender = want.gender ? want.gender : (uint8_t)M;
    std::string part;
    GenInfo pi;
    if (!declineAdjLike(lx, nom, pw, part, macrons, &pi)) return false;
    const uint32_t sum = findLemma(lx, "sum", Verb);
    if (sum == lex::kNoLemma) return false;
    const uint8_t t = want.tense == Perfect ? (uint8_t)Present : want.tense == Pluperfect ? (uint8_t)Imperfect : (uint8_t)Future;
    std::string esse;
    if (!generate(lx, sum, verbForm(want.person, want.number, t, want.mood, Active), esse, macrons, nullptr))
      return false;
    out = part + " " + esse;
    if (info) { info->periphrastic = true; info->fromRule = rule || pi.fromRule; info->lemmaUsed = lemma; info->packed = pack(want); }
    return true;
  }
  // 5. paradigm fallback (lemmas without a table only)
  std::string pf;
  if (detail::paradigmApplies(l) && fromParadigm(l, want, pf, pk)) {
    out = displayForm(pf, macrons);
    if (info) { info->fromRule = true; info->packed = pk; }
    return true;
  }
  return false;
}

}  // namespace vp::morph
