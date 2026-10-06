// Spanish branch of the frame builder (C13): lexicon retagging before the parser and tree normalisation after
// lemmatisation (see spanish.h). Word lists are our own.
#include "spanish.h"

#include <algorithm>
#include <cstring>

#include "vp/features.h"
#include "vp/text.h"

namespace vp::frame::es {

namespace {

using nlp::Token;

bool in(const std::string& w, std::initializer_list<const char*> set) {
  for (const char* s : set)
    if (w == s) return true;
  return false;
}
bool punct(const Token& t) { return t.upos == "PUNCT" || t.upos == "SYM"; }

uint32_t setFeat(uint32_t feats, nlp::morph::Shift s, uint32_t v) {
  const uint32_t mask = (s == nlp::morph::NumberShift || s == nlp::morph::PersonShift) ? 3u
                      : (s == nlp::morph::PronTypeShift ? 15u : 7u);
  return (feats & ~(mask << s)) | ((v & mask) << s);
}
uint32_t nlpTense(uint8_t t) {
  switch (t) {
    case feat::Present: return nlp::morph::TensePres;
    case feat::Imperfect: return nlp::morph::TenseImp;
    case feat::Perfect: return nlp::morph::TensePast;
    case feat::Future: return nlp::morph::TenseFut;
    case feat::Pluperfect: return nlp::morph::TensePqp;
    default: return 0;
  }
}
uint32_t nlpMood(uint8_t m) {
  switch (m) {
    case feat::Indicative: return nlp::morph::MoodInd;
    case feat::Subjunctive: return nlp::morph::MoodSub;
    case feat::Imperative: return nlp::morph::MoodImp;
    default: return 0;
  }
}
bool finite(const VerbReading& r) {
  return r.person && (r.mood == feat::Indicative || r.mood == feat::Subjunctive || r.mood == feat::Imperative);
}

uint32_t featsOf(const VerbReading& r) {
  uint32_t f = 0;
  f = setFeat(f, nlp::morph::VerbFormShift, nlp::morph::VfFin);
  if (r.person) f = setFeat(f, nlp::morph::PersonShift, r.person);
  if (r.number) f = setFeat(f, nlp::morph::NumberShift, r.number == feat::Pl ? nlp::morph::NumPlur : nlp::morph::NumSing);
  if (nlpTense(r.tense)) f = setFeat(f, nlp::morph::TenseShift, nlpTense(r.tense));
  if (nlpMood(r.mood)) f = setFeat(f, nlp::morph::MoodShift, nlpMood(r.mood));
  return f;
}

bool nonVerbReading(const lex::Lexicon& lex, const std::string& lower) {
  std::vector<lex::Analysis> an;
  lex.lookup(text::es_key(lower), an);
  for (const lex::Analysis& a : an) {
    const uint8_t p = lex.lemma(a.lemma).pos;
    if (p == feat::Noun || p == feat::Adj || p == feat::Adv || p == feat::Intj) return true;
  }
  return false;
}

}  // namespace

// ---- word lists -------------------------------------------------------------------------------------------------------
bool personNoun(const std::string& l) {
  return in(l, {"hombre", "mujer", "niño", "niña", "chico", "chica", "muchacho", "muchacha", "persona", "gente",
                "amigo", "amiga", "madre", "padre", "mamá", "papá", "hermano", "hermana", "hijo", "hija", "rey",
                "reina", "príncipe", "princesa", "maestro", "maestra", "profesor", "profesora", "alumno", "alumna",
                "señor", "señora", "señorita", "abuelo", "abuela", "tío", "tía", "primo", "prima", "esposo", "esposa",
                "marido", "doctor", "doctora", "médico", "soldado", "jardinero", "jardinera", "cocinero", "cocinera",
                "agricultor", "campesino", "vecino", "vecina", "dios", "diosa", "bebé", "gato", "gata", "perro",
                "perra", "caballo", "conejo", "pájaro", "ratón", "lobo", "animal", "pez", "todos", "nadie", "alguien",
                "majestad", "duque", "duquesa", "guardia", "criado", "criada", "sirviente", "esclavo", "esclava"});
}

bool motionVerb(const std::string& l) {
  return in(l, {"ir", "venir", "correr", "caminar", "volver", "regresar", "llegar", "andar", "subir", "bajar",
                "entrar", "salir", "caer", "huir", "escapar", "viajar", "volar", "nadar", "saltar", "meter", "poner",
                "llevar", "traer", "enviar", "mandar", "acercar", "marchar", "pasar", "cruzar"});
}

bool intransitiveVerb(const std::string& l) {
  return in(l, {"vivir", "llegar", "venir", "ir", "correr", "dormir", "estar", "existir", "aparecer", "salir",
                "entrar", "caer", "morir", "nacer", "crecer", "sonreír", "reír", "llorar", "volver", "regresar",
                "quedar", "habitar", "esperar", "faltar", "sobrar", "brillar", "sonar", "cantar", "volar", "nadar",
                "caminar", "andar", "pasar", "ocurrir", "suceder", "empezar", "comenzar", "terminar", "acabar"});
}

bool dativeVerb(const std::string& l) {
  return in(l, {"gustar", "encantar", "doler", "importar", "faltar", "parecer", "interesar", "molestar", "quedar",
                "dar", "decir", "contar", "pasar", "mostrar", "enseñar", "traer", "pedir", "preguntar", "escribir",
                "leer", "cantar", "explicar", "regalar", "prestar", "mandar", "enviar", "dejar", "ofrecer",
                "responder", "contestar", "devolver", "entregar", "servir", "tocar", "pertenecer", "convenir",
                "agradar", "apetecer", "costar", "hacer falta", "narrar"});
}

std::string multiwordPrep(const std::string& w) {
  static const std::pair<const char*, const char*> kMap[] = {
      {"detrás", "behind"},  {"delante", "in front of"}, {"cerca", "near"},     {"dentro", "inside"},
      {"encima", "above"},   {"debajo", "below"},        {"alrededor", "around"}, {"lejos", "far from"},
      {"fuera", "out of"},   {"antes", "before"},        {"después", "after"},  {"enfrente", "in front of"},
      {"frente", "in front of"}, {"además", "besides"}, {"junto", "beside"}};
  for (const auto& m : kMap)
    if (w == m.first) return m.second;
  return "";
}

// ---- lexicon readings ---------------------------------------------------------------------------------------------------
void verbReadings(const lex::Lexicon& lex, const std::string& lower, std::vector<VerbReading>& out) {
  out.clear();
  std::vector<lex::Analysis> an;
  lex.lookup(text::es_key(lower), an);
  if (an.empty()) lex.lookup(text::es_bare(lower), an);
  for (const lex::Analysis& a : an) {
    const lex::Lemma l = lex.lemma(a.lemma);
    if (l.id == lex::kNoLemma || l.pos != feat::Verb) continue;
    const feat::Features f = feat::unpack(lex.feature(a.feat));
    VerbReading r;
    r.lemma = text::lower(l.head);
    r.person = f.person;
    r.number = f.number;
    r.tense = f.tense;
    r.mood = f.mood;
    out.push_back(r);
  }
}

bool hasImperative2(const lex::Lexicon& lex, const std::string& lower) {
  std::vector<VerbReading> vr;
  verbReadings(lex, lower, vr);
  for (const VerbReading& r : vr)
    if (r.mood == feat::Imperative && r.person == 2) return true;
  return false;
}

// ---- retag ---------------------------------------------------------------------------------------------------------------
bool retag(std::vector<Token>& tk, const lex::Lexicon& lex) {
  bool changed = false;
  const int n = (int)tk.size();
  bool anyVerb = false;
  int firstWord = -1;
  for (int i = 0; i < n; ++i) {
    anyVerb = anyVerb || tk[(size_t)i].upos == "VERB" || tk[(size_t)i].upos == "AUX";
    if (firstWord < 0 && !punct(tk[(size_t)i])) firstWord = i;
  }
  std::vector<VerbReading> vr;
  for (int i = 0; i < n; ++i) {
    Token& t = tk[(size_t)i];
    if (punct(t)) continue;
    verbReadings(lex, t.lower, vr);
    if (vr.empty()) continue;
    if (t.upos == "VERB" || t.upos == "AUX") {
      // features the tagger left out or got wrong, from the lexicon when its readings agree
      std::vector<uint8_t> tenses, moods;
      uint8_t p0 = 0, n0 = 0;
      bool onePn = true;
      for (const VerbReading& r : vr) {
        if (!finite(r)) continue;
        if (r.mood != feat::Imperative && std::find(tenses.begin(), tenses.end(), r.tense) == tenses.end())
          tenses.push_back(r.tense);
        if (std::find(moods.begin(), moods.end(), r.mood) == moods.end()) moods.push_back(r.mood);
        if (!p0) { p0 = r.person; n0 = r.number; }
        else if (p0 != r.person || n0 != r.number) onePn = false;
      }
      const uint32_t vf = nlp::morph::get(t.feats, nlp::morph::VerbFormShift);
      if (vf == nlp::morph::VfInf || vf == nlp::morph::VfPart || vf == nlp::morph::VfGer) continue;
      if (tenses.empty() && moods.empty()) continue;
      const uint32_t tt = nlp::morph::get(t.feats, nlp::morph::TenseShift);
      bool tagTenseOk = false;
      for (uint8_t x : tenses) tagTenseOk = tagTenseOk || nlpTense(x) == tt;
      if (tenses.size() == 1 && nlpTense(tenses[0]) && !tagTenseOk)
        t.feats = setFeat(t.feats, nlp::morph::TenseShift, nlpTense(tenses[0]));
      const uint32_t md = nlp::morph::get(t.feats, nlp::morph::MoodShift);
      bool tagMoodOk = false;
      for (uint8_t x : moods) tagMoodOk = tagMoodOk || nlpMood(x) == md;
      const bool subj = std::find(moods.begin(), moods.end(), (uint8_t)feat::Subjunctive) != moods.end();
      const bool ind = std::find(moods.begin(), moods.end(), (uint8_t)feat::Indicative) != moods.end();
      if (!tagMoodOk) {
        if (moods.size() == 1) t.feats = setFeat(t.feats, nlp::morph::MoodShift, nlpMood(moods[0]));
        else if (subj && !ind) t.feats = setFeat(t.feats, nlp::morph::MoodShift, nlp::morph::MoodSub);   // entremos
      }
      if (!nlp::morph::get(t.feats, nlp::morph::PersonShift) && onePn && p0) {
        t.feats = setFeat(t.feats, nlp::morph::PersonShift, p0);
        if (n0) t.feats = setFeat(t.feats, nlp::morph::NumberShift, n0 == feat::Pl ? nlp::morph::NumPlur : nlp::morph::NumSing);
      }
      if (!vf) t.feats = setFeat(t.feats, nlp::morph::VerbFormShift, nlp::morph::VfFin);
      continue;
    }
    if (t.upos != "NOUN" && t.upos != "PROPN" && t.upos != "ADJ" && t.upos != "INTJ") continue;
    if (i > 0 && (tk[(size_t)i - 1].upos == "DET" || tk[(size_t)i - 1].upos == "ADP" || tk[(size_t)i - 1].upos == "NUM"))
      continue;
    bool p12 = false, imp = false, fin3 = false;
    for (const VerbReading& r : vr) {
      if (!finite(r)) continue;
      if (r.mood == feat::Imperative) imp = true;
      else if (r.person == 1 || r.person == 2) p12 = true;
      else fin3 = true;
    }
    const bool initial = i == firstWord;
    const bool lexOther = nonVerbReading(lex, t.lower);
    const bool doIt = p12 || (imp && (initial || t.upos == "ADJ" || t.upos == "INTJ" || !lexOther)) ||
                      (initial && t.upos == "PROPN" && (imp || fin3)) || (!anyVerb && fin3 && !lexOther);
    if (!doIt) continue;
    // the reading: an imperative at the start of the sentence, else a 1st/2nd person form, else the first finite
    const VerbReading* best = nullptr;
    for (const VerbReading& r : vr)
      if (finite(r) && r.mood == feat::Imperative && (initial || !p12)) { best = &r; break; }
    if (!best)
      for (const VerbReading& r : vr)
        if (finite(r) && r.mood != feat::Imperative && (r.person == 1 || r.person == 2)) { best = &r; break; }
    if (!best)
      for (const VerbReading& r : vr)
        if (finite(r)) { best = &r; break; }
    if (!best) continue;
    t.upos = "VERB";
    t.feats = featsOf(*best);
    changed = true;
    anyVerb = true;
  }
  return changed;
}

// ---- tree normalisation -------------------------------------------------------------------------------------------------
namespace {

struct Tree {
  std::vector<Token>& tk;
  int n;
  explicit Tree(std::vector<Token>& t) : tk(t), n((int)t.size()) {}
  int head(int i) const { return tk[(size_t)i].head - 1; }   // -1 root / none
  void setHead(int i, int h) { tk[(size_t)i].head = h + 1; }
  int root() const {
    for (int i = 0; i < n; ++i)
      if (tk[(size_t)i].head == 0) return i;
    return -1;
  }
  bool verb(int i) const { return i >= 0 && i < n && (tk[(size_t)i].upos == "VERB" || tk[(size_t)i].upos == "AUX"); }
  void rehangChildren(int from, int to, int except = -1) {
    for (int k = 0; k < n; ++k)
      if (k != to && k != except && head(k) == from) setHead(k, to);
  }
  bool hasChild(int h, const char* rel, int except = -1) const {
    for (int k = 0; k < n; ++k)
      if (k != except && head(k) == h && tk[(size_t)k].deprel == rel) return true;
    return false;
  }
  // a cycle-free re-hang: never make i the descendant of itself
  bool descendant(int a, int of) const {
    int x = a, steps = 0;
    while (x >= 0 && steps <= n) {
      if (x == of) return true;
      x = head(x);
      ++steps;
    }
    return false;
  }
};

uint32_t fget(const Token& t, nlp::morph::Shift s) { return nlp::morph::get(t.feats, s); }

// The main verb a clitic belongs to: proclitic -> the next verb (over other clitics); enclitic -> its host word.
int cliticHost(const Tree& T, int i, const curated::CuratedData& cd) {
  const std::vector<Token>& tk = T.tk;
  if (i > 0 && tk[(size_t)i].start == tk[(size_t)i - 1].start) {   // split off the previous word
    int h = i - 1;
    while (h > 0 && tk[(size_t)h - 1].start == tk[(size_t)i].start) --h;
    return T.verb(h) ? h : -1;
  }
  for (int k = i + 1; k < T.n && k <= i + 3; ++k) {
    const Token& x = tk[(size_t)k];
    if (T.verb(k)) return k;
    if (!cd.clitic(x.lower)) return -1;
  }
  return -1;
}

}  // namespace

void normalise(SemSentence& s, const lex::Lexicon* lex, const curated::CuratedData& cd) {
  std::vector<Token>& tk = s.tokens;
  Tree T(tk);
  const int n = T.n;
  // ---- "por qué" -> why, "a dónde" -> whither ------------------------------------------------------------------------
  for (int i = 0; i + 1 < n; ++i) {
    Token& a = tk[(size_t)i];
    Token& b = tk[(size_t)i + 1];
    if (a.lower == "por" && b.lower == "qué") {
      int r = T.root();
      if (r == i + 1 || r == i || r < 0) {
        r = -1;
        for (int k = 0; k < n && r < 0; ++k)
          if (T.verb(k) && k != i && k != i + 1) r = k;
        if (r < 0) continue;
        T.setHead(r, -1);
        tk[(size_t)r].deprel = "root";
      }
      T.rehangChildren(i + 1, r, i);
      T.rehangChildren(i, r, i + 1);
      b.lower = "por qué";
      b.lemma = "por qué";
      b.upos = "ADV";
      b.deprel = "advmod";
      T.setHead(i + 1, r);
      a.deprel = "fixed";
      T.setHead(i, i + 1);
      continue;
    }
    if (a.lower == "a" && b.lower == "dónde") {
      b.lower = "adónde";
      b.lemma = "adónde";
      if (T.head(i + 1) == i) {   // "dónde" hung on "a": it takes the place of "a"
        T.setHead(i + 1, T.head(i));
        b.deprel = a.deprel == "case" ? "advmod" : a.deprel;
      }
      T.rehangChildren(i, i + 1, i + 1);
      a.deprel = "fixed";
      T.setHead(i, i + 1);
    }
  }
  // ---- multi-word adverbs: "a veces" -> sometimes, "otra vez" / "de nuevo" -> again ... --------------------------------
  static const char* const kAdv2[][3] = {{"a", "veces", "a veces"},       {"otra", "vez", "otra vez"},
                                         {"de", "nuevo", "de nuevo"},     {"por", "fin", "por fin"},
                                         {"a", "menudo", "a menudo"},     {"de", "repente", "de repente"},
                                         {"en", "seguida", "en seguida"}, {"a", "tiempo", "a tiempo"},
                                         {"sin", "embargo", "sin embargo"}, {"tal", "vez", "tal vez"},
                                         {"de", "verdad", "de verdad"},   {"a", "salvo", "a salvo"}};
  for (int i = 0; i + 1 < n; ++i)
    for (const auto& m : kAdv2) {
      if (tk[(size_t)i].lower != m[0] || tk[(size_t)i + 1].lower != m[1]) continue;
      Token& b = tk[(size_t)i + 1];
      int h = T.head(i + 1);
      if (h == i) h = T.head(i);
      // an adverb of a noun belongs to the clause: hang it on the nearest verb
      if (h >= 0 && !T.verb(h)) {
        int v = T.head(h);
        while (v >= 0 && !T.verb(v)) v = T.head(v);
        if (v >= 0) h = v;
      }
      T.rehangChildren(i + 1, h < 0 ? T.root() : h, i);
      b.upos = "ADV";
      b.lemma = m[2];
      b.lower = m[2];
      b.deprel = h < 0 ? "root" : "advmod";
      T.setHead(i + 1, h);
      tk[(size_t)i].deprel = "fixed";
      T.setHead(i, i + 1);
      break;
    }
  // ---- multi-word prepositions: "detrás de la puerta" -> behind ---------------------------------------------------------
  for (int i = 0; i + 1 < n; ++i) {
    if (multiwordPrep(tk[(size_t)i].lower).empty() || tk[(size_t)i + 1].lower != "de") continue;
    int j = -1;
    for (int k = i + 2; k < n; ++k) {
      const std::string& u = tk[(size_t)k].upos;
      if (u == "NOUN" || u == "PROPN" || u == "PRON" || u == "NUM") { j = k; break; }
      if (u != "DET" && u != "ADJ") break;
    }
    if (j < 0) continue;
    const int hi = T.head(i);
    if (T.head(j) == i || T.head(j) == i + 1) {   // the noun hangs on the adverb: it takes the adverb's place
      T.setHead(j, hi == j ? T.root() : hi);
      tk[(size_t)j].deprel = (tk[(size_t)i].deprel == "advmod" || tk[(size_t)i].deprel == "obl") ? "obl" : tk[(size_t)i].deprel;
      if (hi < 0) { tk[(size_t)j].deprel = "root"; T.setHead(j, -1); }
    }
    T.rehangChildren(i, j, i + 1);
    tk[(size_t)i].upos = "ADP";
    tk[(size_t)i].deprel = "case";
    T.setHead(i, j);
    tk[(size_t)i + 1].deprel = "fixed";
    T.setHead(i + 1, i);
  }
  // ---- "un poco de X" -> X with the quantity "a little" -------------------------------------------------------------------
  for (int i = 0; i + 2 < n; ++i) {
    if (!in(tk[(size_t)i].lower, {"un", "una"}) || !in(tk[(size_t)i + 1].lower, {"poco", "poca"}) ||
        tk[(size_t)i + 2].lower != "de")
      continue;
    int j = -1;
    for (int k = i + 3; k < n; ++k) {
      const std::string& u = tk[(size_t)k].upos;
      if (u == "NOUN" || u == "PROPN") { j = k; break; }
      if (u != "DET" && u != "ADJ") break;
    }
    if (j < 0) continue;
    const int p = i + 1;
    int hp = T.head(p);
    std::string rel = tk[(size_t)p].deprel;
    if (hp == j) { hp = T.head(j); rel = tk[(size_t)j].deprel; }
    if (rel == "obl" || rel == "nmod" || rel == "dep") rel = "obj";
    T.setHead(j, hp);
    tk[(size_t)j].deprel = hp < 0 ? "root" : rel;
    T.rehangChildren(p, j, i);
    T.setHead(p, j);
    tk[(size_t)p].deprel = "det";
    tk[(size_t)p].upos = "DET";
    T.setHead(i, j);
    tk[(size_t)i].deprel = "det";
    T.setHead(i + 2, p);
    tk[(size_t)i + 2].deprel = "fixed";
  }
  // ---- clitic pronouns -> arguments, or the particle "se" of a pronominal verb ------------------------------------------
  for (int i = 0; i < n; ++i) {
    Token& c = tk[(size_t)i];
    const curated::CliticEntry* ce = cd.clitic(c.lower);
    if (!ce) continue;
    const int host = cliticHost(T, i, cd);
    if (host < 0) continue;
    if ((c.lower == "la" || c.lower == "las" || c.lower == "lo" || c.lower == "los") && c.upos == "DET" &&
        host != i + 1 && !(i > 0 && c.start == tk[(size_t)i - 1].start))
      continue;
    // the main verb: an auxiliary / modal hands over to its verb
    int main = host;
    if (tk[(size_t)host].upos == "AUX" || tk[(size_t)host].deprel == "aux") {
      const int h = T.head(host);
      if (T.verb(h)) main = h;
      else
        for (int k = host + 1; k < n && k <= host + 3; ++k)
          if (tk[(size_t)k].upos == "VERB" && T.head(k) != k) { main = k; break; }
    }
    // the finite verb that gives the person: the host if finite, else an auxiliary or a governing verb
    int fin = -1;
    for (int k : {host, main, T.head(main), T.head(host)}) {
      if (k < 0 || k >= n || !T.verb(k)) continue;
      if (fget(tk[(size_t)k], nlp::morph::PersonShift) || fget(tk[(size_t)k], nlp::morph::MoodShift) == nlp::morph::MoodImp) {
        fin = k;
        break;
      }
    }
    uint32_t vp = fin >= 0 ? fget(tk[(size_t)fin], nlp::morph::PersonShift) : 0;
    uint32_t vn = fin >= 0 ? fget(tk[(size_t)fin], nlp::morph::NumberShift) : 0;
    if (fin >= 0 && !vp && fget(tk[(size_t)fin], nlp::morph::MoodShift) == nlp::morph::MoodImp) vp = 2;
    const std::string lemma = text::lower(tk[(size_t)main].lemma);
    bool coref = false;
    if (ce->role != "acc" && ce->role != "dat") {
      if (c.lower == "se") coref = !vp || vp == 3;   // also the ustedes imperative ("inclínense")
      else coref = vp == ce->person && (!vn || !ce->number ||
                                        (ce->number == 2) == (vn == nlp::morph::NumPlur));
    }
    // "se lo" / "se la": se stands for le (indirect object)
    const bool seLo = c.lower == "se" && i + 1 < n && in(tk[(size_t)i + 1].lower, {"lo", "la", "los", "las"});
    std::string rel;
    if (coref && !seLo) {
      const bool pronominal = cd.phrasal(lemma, "se") != nullptr;
      if (pronominal) rel = "expl:pv";
      else if (c.lower == "se") {
        // "se" without a pronominal row: passive with an inanimate subject ("Se venden casas"), impersonal (3rd
        // plural) without one, reflexive object otherwise
        int subj = -1, obj = -1;
        for (int k = 0; k < n; ++k)
          if (T.head(k) == main && k != i) {
            if (tk[(size_t)k].deprel == "nsubj") subj = k;
            if (tk[(size_t)k].deprel == "obj") obj = k;
          }
        if (subj >= 0 && tk[(size_t)subj].upos == "NOUN" && !personNoun(text::lower(tk[(size_t)subj].lemma)))
          rel = "expl:pass";
        else if (subj < 0 && obj < 0 && vp == 3 && vn != nlp::morph::NumPlur && !intransitiveVerb(lemma))
          rel = "expl:impers";
        else
          rel = obj >= 0 ? "iobj" : "obj";
      } else {
        rel = "obj";   // "me lavo" -> mē lavō
        for (int k = 0; k < n; ++k)
          if (T.head(k) == main && k != i && tk[(size_t)k].deprel == "obj") rel = "iobj";
      }
    } else if (ce->role == "acc") {
      rel = "obj";
    } else if (ce->role == "dat" || seLo) {
      rel = "iobj";
    } else {
      bool otherObj = false;
      for (int k = 0; k < n; ++k) {
        if (k == i || T.head(k) != main) continue;
        if (tk[(size_t)k].deprel == "obj" && tk[(size_t)k].upos != "PRON") otherObj = true;
        if (tk[(size_t)k].deprel == "ccomp" || tk[(size_t)k].deprel == "xcomp") otherObj = otherObj || dativeVerb(lemma);
      }
      for (int k = 0; k < n; ++k) {   // another clitic of the same verb in the accusative ("dámelo")
        const curated::CliticEntry* o = k != i ? cd.clitic(tk[(size_t)k].lower) : nullptr;
        if (o && o->role == "acc" && cliticHost(T, k, cd) == host) otherObj = true;
      }
      // a noun object the parser left as an oblique or nominal subject of a giving verb ("Cántanos una canción")
      rel = otherObj || dativeVerb(lemma) ? "iobj" : "obj";
    }
    c.upos = "PRON";
    c.deprel = rel;
    if (!T.descendant(main, i)) T.setHead(i, main);
    // other dependents of the clitic (a parser slip) go to the verb
    T.rehangChildren(i, main);
  }
  // ---- personal "a": a person object of a transitive verb is the direct object ("Has visto a mi gato") --------------
  for (int i = 0; i < n; ++i) {
    const Token& a = tk[(size_t)i];
    if (a.lower != "a" || a.deprel != "case") continue;
    const int h = T.head(i);
    if (h < 0) continue;
    const Token& np = tk[(size_t)h];
    if (np.deprel != "obl" && np.deprel != "obj" && np.deprel != "iobj" && np.deprel != "nmod") continue;
    const int v = T.head(h);
    if (!T.verb(v)) continue;
    const std::string vl = text::lower(tk[(size_t)v].lemma);
    const bool person = np.upos == "PROPN" || personNoun(text::lower(np.lemma)) ||
                        (np.upos == "PRON" && in(np.lower, {"él", "ella", "ellos", "ellas", "usted", "ustedes", "mí",
                                                            "ti", "nosotros", "nosotras", "todos", "nadie", "alguien",
                                                            "quién"}));
    if (!person || motionVerb(vl) || dativeVerb(vl) || cd.verbPrep(vl, "to")) continue;
    bool otherObj = false;
    for (int k = 0; k < n; ++k)
      if (k != h && T.head(k) == v && tk[(size_t)k].deprel == "obj") otherObj = true;
    if (otherObj) continue;
    tk[(size_t)h].deprel = "obj";
    tk[(size_t)i].deprel = "mark:a";   // the personal "a" is no preposition
  }
  (void)lex;
}

}  // namespace vp::frame::es
