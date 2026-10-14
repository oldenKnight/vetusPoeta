// Meaning coverage of a readable sentence (C36, calibration): OK must mean right. After the realiser has written the
// English / Spanish sentence, this check compares it with the Latin analysis and flags what a reader would call a
// wrong translation even when every word was analysed with confidence:
//  * meaning-dropped    a content word of the Latin (noun, adjective, participle, verb other than sum, adverb, name)
//                       whose target word (or one of its inflected forms) is not in the output;
//  * meaning-added      a content word in the output that no Latin word accounts for (function words, the readable
//                       tables' closed-class words and the forms of be / ser / estar / haber are not content);
//  * existential-copula a copula clause rendered "there is / hay" although the Latin subject carries a descriptive
//                       adjective and no place is named (Difficilis est vīta: life is hard, not "there is a hard life");
//  * dangling-conjunction  the output ends in "and / but / y / pero ..." (a postpositive or coordinating word lost
//                       its second half);
//  * gender-mismatch    (Spanish) an adjective or participle whose gender contradicts the Latin noun or addressee it
//                       agrees with (electa -> "elegido");
//  * vocative-lost      an addressed noun (a vocative reading in a sentence with a 2nd person verb / pronoun) read as
//                       a bare ablative or dative;
//  * pronoun-guess      (English) a restored "it" for a subject the Latin does not show (no neuter pronoun, no
//                       impersonal verb): the referent is a guess;
//  * article-guess      a first mention after sine rendered with the definite article ("without the master").
// Nothing here changes the text; the flags make the cue Check. Deterministic, no allocation beyond small vectors.
#include <algorithm>
#include <cstring>
#include <functional>

#include "internal.h"
#include "vp/text.h"

namespace vp::la2x::detail {

using namespace vp::feat;

namespace {

bool isKeyC(const std::string& k, std::initializer_list<const char*> ks) {
  for (const char* x : ks)
    if (k == x) return true;
  return false;
}

// lower case, accents folded (Spanish), apostrophes and hyphens kept inside words
std::string fold(std::string_view w) { return text::es_bare(text::lower(w)); }

void words(std::string_view s, std::vector<std::string>& out) {
  out.clear();
  std::string cur;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = i;
    const char32_t c = text::decodeUtf8(s, j);
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0xC0 || c == '\'' || c == '-';
    if (letter && !(c == 0xBF || c == 0xA1 || c == 0x201C || c == 0x201D || c == 0x2019 || c == 0x2018)) {
      cur.append(s.substr(i, j - i));
    } else if (!cur.empty()) {
      out.push_back(fold(cur));
      cur.clear();
    }
    i = j;
  }
  if (!cur.empty()) out.push_back(fold(cur));
  for (std::string& w : out) {
    while (!w.empty() && (w.back() == '\'' || w.back() == '-')) w.pop_back();
    while (!w.empty() && (w[0] == '\'' || w[0] == '-')) w.erase(0, 1);
  }
  out.erase(std::remove(out.begin(), out.end(), std::string()), out.end());
}

const char* const kStopEn[] = {
    "a", "an", "the", "and", "or", "but", "nor", "so", "for", "yet", "not", "no", "do", "does", "did", "don't",
    "doesn't", "didn't", "is", "am", "are", "was", "were", "be", "been", "being", "will", "would", "shall", "should",
    "can", "could", "cannot", "must", "may", "might", "have", "has", "had", "having", "to", "of", "in", "on", "at", "by",
    "with", "without", "from", "into", "onto", "out", "up", "down", "over", "under", "than", "as", "that", "this",
    "these", "those", "there", "here", "it", "its", "he", "him", "his", "she", "her", "hers", "they", "them", "their",
    "theirs", "we", "us", "our", "ours", "you", "your", "yours", "i", "me", "my", "mine", "myself", "yourself",
    "himself", "herself", "itself", "ourselves", "yourselves", "themselves", "who", "whom", "whose", "which", "what",
    "when", "where", "why", "how", "one", "ones", "very", "more", "most", "let", "also", "too", "then", "now", "o",
    "oh", "after", "while", "order", "if", "because", "although", "though", "about", "isn't", "aren't", "wasn't",
    "weren't", "can't", "won't", "able", "usually", "used", "surely", "much", "many", "all", "some", "any", "each",
    "every", "both", "either", "neither", "other", "another", "such", "same", "own", "only", "just", "until", "before",
    "since", "through", "between", "among", "against", "around", "behind", "beside", "near", "across", "towards",
    "toward", "during", "upon", "within", "never", "always", "often", "again", "even", "still", "already", "please"};
const char* const kStopEs[] = {
    "el", "la", "los", "las", "lo", "un", "una", "unos", "unas", "y", "e", "o", "u", "ni", "pero", "sino", "que", "de",
    "del", "a", "al", "en", "con", "sin", "por", "para", "no", "si", "se", "me", "te", "le", "les", "nos", "os", "mi",
    "mis", "tu", "tus", "su", "sus", "nuestro", "nuestra", "nuestros", "nuestras", "vuestro", "vuestra", "yo", "tu",
    "el", "ella", "ello", "ellos", "ellas", "nosotros", "nosotras", "ustedes", "usted", "mio", "mia", "tuyo", "tuya",
    "suyo", "suya", "este", "esta", "esto", "estos", "estas", "ese", "esa", "eso", "esos", "esas", "aquel", "aquella",
    "aquello", "aquellos", "aquellas", "quien", "quienes", "cual", "cuales", "como", "cuando", "donde", "adonde",
    "porque", "pues", "muy", "mas", "menos", "tan", "tanto", "tambien", "ya", "oh", "hay", "habia", "habra", "hubo",
    "uno", "otro", "otra", "otros", "otras", "todo", "toda", "todos", "todas", "mucho", "mucha", "muchos", "muchas",
    "acaso", "mismo", "misma", "asi", "aqui", "alli", "ahi", "entonces", "luego", "ahora", "nunca", "siempre",
    "solo", "sola", "hasta", "desde", "entre", "sobre", "hacia", "contra", "segun", "durante", "mientras", "aunque",
    "despues", "antes", "ti", "mi", "si", "conmigo", "contigo", "consigo"};

bool inList(const std::string& w, const char* const* list, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (w == list[i]) return true;
  return false;
}

}  // namespace

struct Coverage::Impl {
  std::vector<std::string> stopEn, stopEs;   // sorted, folded
  bool isStop(const std::string& w, Target t) const {
    const std::vector<std::string>& v = t == Target::En ? stopEn : stopEs;
    return std::binary_search(v.begin(), v.end(), w);
  }
};

Coverage::Coverage() : impl_(std::make_unique<Impl>()) {}
Coverage::~Coverage() = default;

void Coverage::init(const Tables& en, const Tables& es) {
  auto build = [](const Tables& tab, const char* const* list, size_t n, bool spanish, std::vector<std::string>& out) {
    out.clear();
    for (size_t i = 0; i < n; ++i) out.push_back(fold(list[i]));
    std::vector<std::string> w;
    for (const Row& r : tab.rows()) {
      if (r.kind == "lex" || r.kind == "lexh" || r.kind == "name") continue;
      words(r.text, w);
      out.insert(out.end(), w.begin(), w.end());
    }
    if (spanish) {
      // every form of ser / estar / haber / ir (auxiliaries and the copula the realiser adds)
      using es::VTense;
      for (const char* v : {"ser", "estar", "haber", "ir"})
        for (VTense t : {VTense::Present, VTense::Imperfect, VTense::Preterite, VTense::Future, VTense::Conditional,
                         VTense::SubjPresent, VTense::SubjImperfect, VTense::Imperative, VTense::PastPart,
                         VTense::Gerund, VTense::Infinitive})
          for (int p = 1; p <= 3; ++p)
            for (int nmb = 1; nmb <= 2; ++nmb) out.push_back(fold(es::verb(v, t, p, nmb)));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
  };
  build(en, kStopEn, sizeof(kStopEn) / sizeof(kStopEn[0]), false, impl_->stopEn);
  build(es, kStopEs, sizeof(kStopEs) / sizeof(kStopEs[0]), true, impl_->stopEs);
}

namespace {

// Forms a target word may take in the output (folded). `pos`: the Latin reading's part of speech.
void formsOf(const std::string& word, uint8_t pos, bool participle, Target t, std::vector<std::string>& out) {
  out.clear();
  if (word.empty()) return;
  std::string w = word;
  // the first content word of a multi-word gloss carries the inflection ("have pity on", "estar sentado")
  std::vector<std::string> parts;
  words(w, parts);
  if (parts.empty()) return;
  for (const std::string& p : parts) out.push_back(p);
  if (t == Target::En) {
    for (const std::string& p : parts) {
      out.push_back(fold(en::plural(p)));
      out.push_back(fold(en::comparative(p)));
      out.push_back(fold(en::superlative(p)));
      for (en::VForm f : {en::VForm::S3, en::VForm::Past, en::VForm::PastPart, en::VForm::Ing})
        out.push_back(fold(en::verb(p, f)));
    }
  } else {
    using es::VTense;
    const bool verb = pos == Verb || participle;
    const std::string inf = parts[0];
    if (verb && inf.size() > 2 &&
        (inf.compare(inf.size() - 2, 2, "ar") == 0 || inf.compare(inf.size() - 2, 2, "er") == 0 ||
         inf.compare(inf.size() - 2, 2, "ir") == 0 || inf.compare(inf.size() - 2, 2, "se") == 0)) {
      for (VTense tt : {VTense::Present, VTense::Imperfect, VTense::Preterite, VTense::Future, VTense::Conditional,
                        VTense::SubjPresent, VTense::SubjImperfect, VTense::Imperative, VTense::PastPart,
                        VTense::Gerund, VTense::Infinitive})
        for (int p = 1; p <= 3; ++p)
          for (int n = 1; n <= 2; ++n) {
            std::vector<std::string> fw;
            words(es::verb(word, tt, p, n), fw);
            if (!fw.empty()) out.push_back(fw[0]);
          }
      const std::string pp = es::verb(word, VTense::PastPart, 3, 1);
      std::vector<std::string> fw;
      for (uint8_t g : {(uint8_t)M, (uint8_t)F})
        for (uint8_t n : {(uint8_t)1, (uint8_t)2}) {
          words(es::adjective(pp, g, n), fw);
          out.insert(out.end(), fw.begin(), fw.end());
        }
    }
    for (const std::string& p : parts) {
      out.push_back(fold(es::plural(p)));
      for (uint8_t g : {(uint8_t)M, (uint8_t)F})
        for (uint8_t n : {(uint8_t)1, (uint8_t)2}) out.push_back(fold(es::adjective(p, g, n)));
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
}

// an output word `o` realises the form `f`: equal, or `f` with clitics attached (consuélame, dámelo), or an
// inflection the generator does not know that keeps a long stem (happy / happily)
bool realises(const std::string& o, const std::string& f, Target t) {
  if (o == f) return true;
  if (f.size() < 3) return false;
  if (t == Target::Es && o.size() > f.size() && o.size() - f.size() <= 6 && o.compare(0, f.size(), f) == 0) {
    const std::string rest = o.substr(f.size());
    for (const char* c : {"me", "te", "se", "lo", "la", "le", "nos", "los", "las", "les", "melo", "mela", "selo",
                          "sela", "telo", "tela", "noslo", "nosla"})
      if (rest == c) return true;
  }
  size_t k = 0;
  while (k < o.size() && k < f.size() && o[k] == f[k]) ++k;
  return k >= 5 && k + 2 >= f.size() && k + 3 >= o.size();
}

}  // namespace

void Coverage::check(const CoverageIn& in, const std::string& text, std::vector<std::string>& flags,
                     std::vector<std::string>& notes) const {
  const Sentence& s = *in.s;
  const std::vector<TokInfo>& ti = *in.ti;
  const Built& b = *in.b;
  const Target t = in.lang;
  auto flag = [&](const char* f, const std::string& note) {
    if (std::find(flags.begin(), flags.end(), f) == flags.end()) flags.push_back(f);
    if (!note.empty()) notes.push_back(std::string(f) + ": " + note);
  };
  std::vector<std::string> out;
  words(text, out);
  std::vector<char> accounted(out.size(), 0);
  for (size_t k = 0; k < out.size(); ++k)
    if (impl_->isStop(out[k], t)) accounted[k] = 1;

  // ---- dropped content words ----
  std::vector<std::string> forms, extra;
  for (size_t i = 0; i < s.tokens.size() && i < ti.size(); ++i) {
    const TokInfo& x = ti[i];
    if (!x.word || s.tokens[i].unknown) continue;
    const std::string role = i < b.roles.size() ? b.roles[i] : std::string();
    if (isKeyC(role, {"phrase", "interjection", "connector", "negation", "question", "question word", "preposition",
                      "and", "comparison", "intensifier", "prohibition", "numeral", "exclamation"}))
      continue;
    const uint8_t pos = x.f.pos ? x.f.pos : x.lpos;
    const bool participle = pos == Participle || x.lpos == Participle || (pos == Verb && x.f.mood == ParticipleMood);
    std::string word;
    if (x.name) {
      word = in.lex->nameIn(x, t);
    } else if (pos == Noun || participle || (pos == Adj && !in.tab->has("det", x.key)) ||
               (pos == Verb && !isKeyC(x.key, {"sum", "possum", "debeo", "soleo", "nolo", "quaeso", "amabo"}))) {
      word = in.lex->lexical(x, t).word;
    } else if (pos == Adv || x.lpos == Adv) {
      const std::string tk = text::latin_key(s.tokens[i].text);
      if (in.tab->has("adv", tk) || in.tab->has("adv", x.key) || in.tab->has("wh", tk)) continue;   // closed class
      word = in.lex->lexical(x, t).word;
    } else {
      continue;
    }
    if (word.empty() || word[0] == '[') continue;   // an unknown word is already Fix / Check
    formsOf(word, pos, participle, t, forms);
    // other rows of the same key (verb+obj, verb+wh, verb+refl, adj rows of participles) are the same word
    for (const Row& r : in.tab->rows()) {
      if ((r.kind != "lex" && r.kind != "lexh") || r.latin != x.key) continue;
      formsOf(r.text, pos, participle, t, extra);
      forms.insert(forms.end(), extra.begin(), extra.end());
    }
    bool found = false;
    for (size_t k = 0; k < out.size(); ++k)
      for (const std::string& f : forms)
        if (!impl_->isStop(f, t) && realises(out[k], f, t)) {
          accounted[k] = 1;
          found = true;
        }
    // a gloss made only of function words ("be eager" -> eager is content; "be" alone counts as found)
    bool allStop = true;
    for (const std::string& f : forms) allStop = allStop && impl_->isStop(f, t);
    if (!found && !allStop) flag("meaning-dropped", s.tokens[i].text);
  }
  // ---- added content words ----
  for (size_t k = 0; k < out.size(); ++k)
    if (!accounted[k]) flag("meaning-added", out[k]);

  // ---- existential for a copula with a descriptive adjective ----
  std::function<void(const frame::SemFrame&)> walk = [&](const frame::SemFrame& f) {
    if (f.existential && f.hasSubject && f.obliques.empty()) {
      bool descriptive = false;
      for (const frame::SemAdj& a : f.subject.adjectives)
        if (!isKeyC(a.lemma, {"multus", "nullus", "paucus", "omnis", "aliquis", "ullus", "tantus", "quantus"}))
          descriptive = true;
      bool place = false;
      for (const frame::SemAdverb& a : f.adverbs) place = place || isKeyC(a.lemma, {"hic", "ibi", "illic", "ubi"});
      if (descriptive && !place) flag("existential-copula", "");
    }
    for (const frame::SemSub& ss : f.subordinate)
      for (const frame::SemFrame& g : ss.frame) walk(g);
  };
  for (const frame::SemFrame& f : b.frames) walk(f);

  // ---- dangling conjunction ----
  if (!out.empty()) {
    const std::string& last = out.back();
    if ((t == Target::En && isKeyC(last, {"and", "but", "or", "nor"})) ||
        (t == Target::Es && isKeyC(last, {"y", "e", "o", "u", "pero", "sino", "ni"})))
      flag("dangling-conjunction", last);
  }

  // ---- addressee read as an oblique case ----
  bool second = false;
  for (size_t i = 0; i < s.tokens.size() && i < ti.size(); ++i) {
    const TokInfo& x = ti[i];
    if (!x.word) continue;
    if (x.f.pos == Verb && x.f.person == 2 && (x.f.mood == Imperative || x.f.mood == Indicative || x.f.mood == Subjunctive))
      second = true;
    if (isKeyC(x.key, {"tu", "uos"})) second = true;
  }
  if (second)
    for (size_t i = 0; i < s.tokens.size() && i < ti.size(); ++i) {
      const TokInfo& x = ti[i];
      if (!x.word || !(x.f.case_ == Abl || x.f.case_ == Dat)) continue;
      const uint8_t pos = x.f.pos ? x.f.pos : x.lpos;
      if (pos != Noun && pos != Name && !x.name) continue;
      if (i > 0 && ti[i - 1].lpos == Prep) continue;
      bool vocReading = false;
      for (const Reading& r : s.tokens[i].readings) vocReading = vocReading || unpack(r.packed).case_ == Voc;
      if (vocReading && s.tokens[i].capitalised) flag("vocative-lost", s.tokens[i].text);
    }

  // ---- Spanish: gender of adjectives and participles ----
  if (t == Target::Es) {
    for (size_t i = 0; i < s.tokens.size() && i < ti.size(); ++i) {
      const TokInfo& x = ti[i];
      if (!x.word || x.name) continue;
      const uint8_t pos = x.f.pos ? x.f.pos : x.lpos;
      const bool participle = pos == Participle || x.lpos == Participle || (pos == Verb && x.f.mood == ParticipleMood);
      if (!(participle || pos == Adj) || in.tab->has("det", x.key)) continue;
      if (x.f.gender != M && x.f.gender != F) continue;
      // the noun it agrees with: its Spanish gender decides; else (a pronoun, the addressee) the Latin gender
      uint8_t g = x.f.gender;
      int bestD = 99;
      for (size_t j = 0; j < s.tokens.size() && j < ti.size(); ++j) {
        const TokInfo& y = ti[j];
        const uint8_t yp = y.f.pos ? y.f.pos : y.lpos;
        if (j == i || !y.word || y.name || yp != Noun) continue;
        if (y.f.case_ != x.f.case_ || (y.f.number && x.f.number && y.f.number != x.f.number)) continue;
        const uint8_t yg = y.f.gender ? y.f.gender : y.lgender;
        if (yg != x.f.gender && yg != MF && yg != MFN) continue;
        const int d = (int)(j > i ? j - i : i - j);
        if (d < bestD) {
          bestD = d;
          const Lexical ly = in.lex->lexical(y, t);
          g = ly.gender ? ly.gender : x.f.gender;
        }
      }
      const Lexical lx = in.lex->lexical(x, t);
      if (lx.word.empty() || lx.word[0] == '[') continue;
      std::string base = lx.word;
      if (participle && !lx.adjective) base = es::verb(lx.word, es::VTense::PastPart, 3, 1);
      const uint8_t n = x.f.number == Pl ? 2 : 1;
      const std::string right = fold(es::adjective(base, g, n)), wrong = fold(es::adjective(base, g == F ? M : F, n));
      if (right == wrong) continue;
      std::vector<std::string> rw, ww;
      words(right, rw);
      words(wrong, ww);
      if (rw.empty() || ww.empty()) continue;
      const bool hasRight = std::find(out.begin(), out.end(), rw.back()) != out.end();
      const bool hasWrong = std::find(out.begin(), out.end(), ww.back()) != out.end();
      if (hasWrong && !hasRight) flag("gender-mismatch", s.tokens[i].text);
    }
  }

  // ---- English: a restored "it" nobody wrote ----
  if (t == Target::En && std::find(out.begin(), out.end(), "it") != out.end()) {
    bool neuterSource = false;
    for (size_t i = 0; i < ti.size(); ++i) {
      const TokInfo& x = ti[i];
      if (!x.word) continue;
      if (isKeyC(x.key, {"is", "hic", "ille", "iste", "id", "ipse", "idem", "quis", "quid", "qui", "res"}) &&
          (x.f.gender == N || x.f.gender == 0))
        neuterSource = true;
      if (isKeyC(x.key, {"pluo", "pluit", "ningit", "tonat", "fulget", "ningo", "tono", "fulgeo", "fulmino", "licet",
                         "oportet", "decet", "paenitet", "piget", "pudet", "taedet", "lucesco", "aduesperascit",
                         "aduesperasco"}))
        neuterSource = true;
    }
    bool implicit3 = false;
    std::function<void(const frame::SemFrame&)> imp = [&](const frame::SemFrame& f) {
      if (f.hasSubject && f.implicitSubject && (f.subject.pron.person == 3 || f.subject.pron.person == 0) &&
          f.subject.pron.number != 2)
        implicit3 = true;
      for (const frame::SemSub& ss : f.subordinate)
        for (const frame::SemFrame& g : ss.frame) imp(g);
    };
    for (const frame::SemFrame& f : b.frames) imp(f);
    bool phrase = false;
    for (const std::string& r : b.roles) phrase = phrase || r == "phrase";
    if (implicit3 && !neuterSource && !phrase) flag("pronoun-guess", "it");
  }

  // ---- a first mention after sine with the definite article ----
  for (size_t i = 0; i + 1 < s.tokens.size() && i + 1 < ti.size(); ++i) {
    if (ti[i].key != "sine" || ti[i].lpos != Prep) continue;
    const TokInfo& y = ti[i + 1];
    if (!y.word || y.name || y.f.number == Pl) continue;
    const uint8_t yp = y.f.pos ? y.f.pos : y.lpos;
    if (yp != Noun || !in.mentionedBefore) continue;
    if (std::find(in.mentionedBefore->begin(), in.mentionedBefore->end(), y.lemma) != in.mentionedBefore->end()) continue;
    const Lexical ly = in.lex->lexical(y, t);
    if (ly.unique || ly.mass) continue;
    std::vector<std::string> nw;
    words(ly.word, nw);
    if (nw.empty()) continue;
    const char* const* arts = nullptr;
    static const char* const kEn[] = {"the"};
    static const char* const kEs[] = {"el", "la"};
    const size_t na = t == Target::En ? 1 : 2;
    arts = t == Target::En ? kEn : kEs;
    for (size_t k = 0; k + 2 < out.size(); ++k) {
      if (out[k] != (t == Target::En ? "without" : "sin")) continue;
      if (inList(out[k + 1], arts, na) && out[k + 2].compare(0, std::min<size_t>(4, nw[0].size()), nw[0], 0,
                                                            std::min<size_t>(4, nw[0].size())) == 0)
        flag("article-guess", out[k + 1] + " " + out[k + 2]);
    }
  }
}

}  // namespace vp::la2x::detail

namespace vp::la2x::detail {
bool Coverage::isCoverageFlag(std::string_view f) {
  for (const char* x : {"meaning-dropped", "meaning-added", "existential-copula", "dangling-conjunction",
                        "gender-mismatch", "vocative-lost", "pronoun-guess", "article-guess"})
    if (f == x) return true;
  return false;
}
}  // namespace vp::la2x::detail
