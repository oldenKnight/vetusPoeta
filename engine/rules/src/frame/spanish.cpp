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

bool notDiminutive(const std::string& l) {
  return in(l, {"señorita", "carretilla", "palito", "mosquito", "cerito", "favorito", "favorita", "bonito", "bonita",
                "maldito", "maldita", "chiquito", "chiquita", "manito", "requisito", "apetito", "delito", "grito",
                "mito", "rito", "cita", "visita", "pepita", "margarita", "infinito", "exquisito", "solicitud",
                "momentito", "ratito", "poquito", "cerquita", "ahorita"});
}

std::string mascSingular(const std::string& w0) {
  std::string w = w0;
  if (w.size() > 3 && w.back() == 's' && (w[w.size() - 2] == 'a' || w[w.size() - 2] == 'o')) w.pop_back();
  if (w.size() > 3 && w.back() == 'a') w.back() = 'o';
  return w;
}

bool addressNoun(const std::string& l) {
  return in(l, {"mamá", "papá", "mami", "papi", "mamita", "papito", "madre", "padre", "abuela", "abuelo", "abuelita",
                "abuelito", "hijo", "hija", "hijos", "hijas", "niño", "niña", "niños", "niñas", "maestra", "maestro",
                "maestros", "señora", "señor", "señorita", "señoras", "señores", "hermano", "hermana", "hermanito",
                "hermanita", "tía", "tío", "amigo", "amiga", "amigos", "amigas", "chicos", "chicas", "muchachos",
                "muchachas", "doctor", "doctora", "profesor", "profesora", "joven", "jóvenes", "cariño", "amor",
                "mijo", "mija", "abuelos", "papás"});
}

bool endearment(const std::string& base) {
  return in(base, {"abuela", "abuelo", "mamá", "papá", "madre", "padre", "mama", "papa", "hija", "hijo", "tía", "tío"});
}

std::string diminutiveBase(const lex::Lexicon& lex, const std::string& w0) {
  if (notDiminutive(w0)) return "";
  std::string w = w0;
  if (w.size() > 6 && w.back() == 's' && (w[w.size() - 2] == 'o' || w[w.size() - 2] == 'a')) w.pop_back();
  auto ends = [&](const char* suf) {
    const size_t k = std::strlen(suf);
    return w.size() > k + 2 && w.compare(w.size() - k, k, suf) == 0;
  };
  for (const char* suf : {"ecito", "ecita", "cito", "cita", "ito", "ita"}) {
    if (!ends(suf)) continue;
    std::string stem = w.substr(0, w.size() - std::strlen(suf));
    const char g = w.back();   // o / a
    std::vector<std::string> bases = {stem + g, stem + (g == 'o' ? "a" : "o"), stem, stem + "e"};
    if (stem.size() > 2 && stem.compare(stem.size() - 2, 2, "gu") == 0) {   // amiguito -> amigo, tortuguita -> tortuga
      const std::string s2 = stem.substr(0, stem.size() - 1);
      bases.insert(bases.begin(), s2 + g);
    } else if (stem.size() > 2 && stem.compare(stem.size() - 2, 2, "qu") == 0) {   // vaquita -> vaca
      const std::string s2 = stem.substr(0, stem.size() - 2) + "c";
      bases.insert(bases.begin(), s2 + g);
    }
    for (const std::string& b : bases) {
      std::vector<lex::Analysis> an;
      lex.lookup(text::es_key(b), an);
      if (an.empty()) lex.lookup(text::es_bare(b), an);
      for (const lex::Analysis& a : an) {
        const lex::Lemma l = lex.lemma(a.lemma);
        if (l.pos == feat::Noun) return text::es_bare(text::lower(l.head)) == text::es_bare(b) ? text::lower(l.head) : b;
      }
    }
    break;
  }
  return "";
}

bool experiencerVerb(const std::string& l) {
  return in(l, {"gustar", "encantar", "doler", "importar", "faltar", "parecer", "interesar", "molestar", "agradar",
                "apetecer", "quedar", "sobrar", "tocar"});
}

bool rareLemma(const std::string& l) {
  // homograph lemmas the lexicon lists next to a common verb with the same forms (our own list)
  return in(l, {"dolar", "vetar", "rotar", "profundar", "extrañar", "erar", "rosar", "ere", "pelotar",
                "rosarse", "crear"});
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
namespace {
bool capital(const std::string& w) { return !w.empty() && ((w[0] >= 'A' && w[0] <= 'Z') || w.compare(0, 2, "\xC3\x81") == 0 ||
                                                          w.compare(0, 2, "\xC3\x89") == 0 || w.compare(0, 2, "\xC3\x93") == 0); }
bool nounReading(const lex::Lexicon& lex, const std::string& lower, bool* plural = nullptr) {
  std::vector<lex::Analysis> an;
  lex.lookup(text::es_key(lower), an);
  for (const lex::Analysis& a : an)
    if (lex.lemma(a.lemma).pos == feat::Noun) {
      if (plural) *plural = feat::unpack(lex.feature(a.feat)).number == feat::Pl;
      return true;
    }
  return false;
}

}  // namespace

// C34: words the tagger reads as verbs or adjectives that the tables and the lexicon read otherwise: a name of
// names_la.tsv ("Mateo" is not matear), a word of address before a comma ("Mamá, ...": not mamar), a diminutive the
// lexicon does not list ("tortuguita"), "la vi" (the clitic and ver, not an article and a noun), a sentence-initial
// "Ven" before "a" / "aquí" / a comma (venir's imperative, not ver 3rd plural)
bool retagWords(std::vector<Token>& tk, const lex::Lexicon& lex, const curated::CuratedData& cd, bool& guess) {
  bool changed = false;
  const int n = (int)tk.size();
  int firstWord = -1;
  for (int i = 0; i < n && firstWord < 0; ++i)
    if (!punct(tk[(size_t)i])) firstWord = i;
  auto setNoun = [&](Token& t, const char* upos, bool plural) {
    if (t.upos == upos) return;
    t.upos = upos;
    t.feats = nlp::morph::fromString(plural ? "Number=Plur" : "Number=Sing");
    changed = true;
  };
  for (int i = 0; i < n; ++i) {
    Token& t = tk[(size_t)i];
    if (punct(t) || t.text.empty()) continue;
    const std::string low = t.lower.empty() ? nlp::normalise(t.text) : t.lower;
    const bool next = i + 1 < n && (tk[(size_t)i + 1].text == "," || tk[(size_t)i + 1].text == "!" ||
                                    tk[(size_t)i + 1].text == "?" || tk[(size_t)i + 1].text == ".");
    const bool afterComma = i > 0 && tk[(size_t)i - 1].text == ",";
    // "hace tres días", "hace una hora": the time before now ("ago"), a preposition of the time noun, not the verb
    if (in(low, {"hace", "hacía"}) && i + 2 < n &&
        (tk[(size_t)i + 1].upos == "NUM" || in(tk[(size_t)i + 1].lower, {"un", "una", "unos", "unas", "dos", "tres", "mucho",
                                                                          "muchos", "mucha", "muchas", "poco", "pocos"})) &&
        in(tk[(size_t)i + 2].lower, {"día", "días", "hora", "horas", "semana", "semanas", "mes", "meses", "año", "años",
                                     "minuto", "minutos", "rato", "momento", "tiempo", "noche", "noches"})) {
      if (t.upos != "ADP") {
        t.upos = "ADP";
        t.feats = 0;
        changed = true;
      }
      continue;
    }
    // a name of names_la.tsv, capitalised (not a sentence-initial common word with a reading of its own)
    if (capital(t.text) && cd.nameByEnglish(t.text) && t.upos != "PROPN" &&
        (i != firstWord || next || !nounReading(lex, low))) {
      setNoun(t, "PROPN", false);
      continue;
    }
    // a word of address alone between the sentence start / a comma and a comma / the end
    if ((i == firstWord || afterComma || (i > 0 && tk[(size_t)i - 1].text == "\xC2\xA1")) && next &&
        t.upos != "NOUN" && t.upos != "PROPN") {
      std::string base = diminutiveBase(lex, low);
      bool plural = false;
      if ((addressNoun(low) || (!base.empty() && addressNoun(base))) && (nounReading(lex, low, &plural) || !base.empty())) {
        setNoun(t, "NOUN", plural || (low.size() > 2 && low.back() == 's'));
        continue;
      }
    }
    if (t.upos == "PROPN" && !capital(t.text)) continue;
    if (t.upos == "PROPN" && i == firstWord && next && addressNoun(low) && nounReading(lex, low)) {   // "Niños, ..."
      setNoun(t, "NOUN", low.back() == 's');
      continue;
    }
    // a diminutive the lexicon does not list: the noun of its base
    if (t.upos != "NOUN" && t.upos != "PROPN") {
      std::vector<lex::Analysis> an;
      lex.lookup(text::es_key(low), an);
      if (an.empty() && !diminutiveBase(lex, low).empty()) {
        setNoun(t, "NOUN", low.back() == 's');
        continue;
      }
    }
    // "¿Está usted enfermo?", "Estoy cansado": a noun-tagged word after estar (and its subject pronoun) that the
    // lexicon also reads as an adjective is the predicate adjective
    if (t.upos == "NOUN" && i > 0) {
      int p = i - 1;
      if (p > 0 && in(tk[(size_t)p].lower, {"usted", "ustedes", "tú", "yo", "él", "ella", "nosotros", "ellos", "ellas"})) --p;
      const bool estar = in(tk[(size_t)p].lower, {"está", "estás", "estoy", "estamos", "están", "estaba", "estabas",
                                                  "estaban", "estábamos", "estuvo", "estuve", "estuviste"});
      if (estar) {
        std::vector<lex::Analysis> an;
        lex.lookup(text::es_key(low), an);
        bool adj = false;
        for (const lex::Analysis& a : an) adj = adj || lex.lemma(a.lemma).pos == feat::Adj;
        if (adj) {
          t.upos = "ADJ";
          t.feats = 0;
          changed = true;
          continue;
        }
      }
    }
    // "la vi": a clitic tagged as an article before a 1st-person preterite tagged as a noun
    if (i > 0 && t.upos == "NOUN" && in(tk[(size_t)i - 1].lower, {"lo", "la", "los", "las"}) && !nounReading(lex, low)) {
      std::vector<VerbReading> vr;
      verbReadings(lex, low, vr);
      for (const VerbReading& r : vr)
        if (finite(r) && r.mood == feat::Indicative) {
          t.upos = "VERB";
          t.feats = featsOf(r);
          tk[(size_t)i - 1].upos = "PRON";
          tk[(size_t)i - 1].feats = 0;
          changed = true;
          break;
        }
      continue;
    }
    // "Ven a la mesa.", "Ven aquí.", "Ven, ...": venir's imperative at the start of the sentence
    if (i == firstWord && (t.upos == "VERB" || t.upos == "AUX") && i + 1 < n &&
        nlp::morph::get(t.feats, nlp::morph::MoodShift) != nlp::morph::MoodImp && hasImperative2(lex, low) &&
        (in(tk[(size_t)i + 1].lower, {"a", "al", "aquí", "acá", "conmigo", "pronto", "rápido", ","}) || tk[(size_t)i + 1].text == "!" ||
         tk[(size_t)i + 1].text == ".")) {
      std::vector<VerbReading> vr;
      verbReadings(lex, low, vr);
      bool third = false;
      for (const VerbReading& r : vr) third = third || (finite(r) && r.mood != feat::Imperative && r.person == 3 && r.number == feat::Pl);
      const uint32_t tp = nlp::morph::get(t.feats, nlp::morph::PersonShift);
      if (third && (tp == 3 || tp == 0))
        for (const VerbReading& r : vr)
          if (r.mood == feat::Imperative && r.person == 2) {
            t.feats = featsOf(r);
            t.upos = "VERB";
            changed = true;
            guess = true;   // the reading is a guess from the word order (Check), as the C13 retag
            break;
          }
    }
  }
  return changed;
}

namespace {
bool capital(const std::string& w);
}
bool retag(std::vector<Token>& tk, const lex::Lexicon& lex, const curated::CuratedData* cd) {
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
    if (cd && capital(t.text) && cd->nameByEnglish(t.text)) continue;   // C34: a name of names_la.tsv ("Mateo", not matear)
    if (t.upos == "ADJ") {   // "Mi padre es agricultor": a word the lexicon knows only as a noun
      std::vector<lex::Analysis> an;
      lex.lookup(text::es_key(t.lower), an);
      bool noun = false, other = false;
      for (const lex::Analysis& a : an) {
        const uint8_t p = lex.lemma(a.lemma).pos;
        if (p == feat::Noun) noun = true;
        else if (p != feat::Name) other = true;
      }
      if (noun && !other) { t.upos = "NOUN"; changed = true; continue; }
    }
    verbReadings(lex, t.lower, vr);
    if (vr.empty()) continue;
    if (t.upos == "VERB" && i > 0) {
      // a participle after estar / ser ("está roto": not rotar 1sg); an adjective after a noun ("un hoyo profundo":
      // not profundar 1sg)
      std::vector<lex::Analysis> an;
      lex.lookup(text::es_key(t.lower), an);
      bool part = false, adj = false;
      for (const lex::Analysis& a : an) {
        const lex::Lemma l = lex.lemma(a.lemma);
        if (l.pos == feat::Verb && feat::unpack(lex.feature(a.feat)).mood == feat::ParticipleMood) part = true;
        if (l.pos == feat::Adj) adj = true;
      }
      const Token& pv = tk[(size_t)i - 1];
      bool p3 = false;
      for (const VerbReading& r : vr) p3 = p3 || (finite(r) && r.person != 1);
      if (part && (pv.upos == "AUX" || pv.upos == "VERB") &&
          in(pv.lower, {"está", "estás", "estoy", "estamos", "están", "estaba", "estaban", "estabas", "es", "son", "era",
                        "eran", "fue", "fueron", "estuvo"})) {
        t.feats = setFeat(0, nlp::morph::VerbFormShift, nlp::morph::VfPart);
        if (nlp::morph::get(t.feats, nlp::morph::NumberShift) == 0) t.feats = setFeat(t.feats, nlp::morph::NumberShift, nlp::morph::NumSing);
        continue;
      }
      if (adj && !p3 && (pv.upos == "NOUN" || pv.upos == "ADV")) {
        t.upos = "ADJ";
        t.feats = 0;
        changed = true;
        continue;
      }
    }
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
      // C34: the lexicon's one person / number wins over the tagger's ("¿Dormiste bien?", "¿Qué hiciste?": 2nd
      // singular, not 3rd)
      const uint32_t tp = nlp::morph::get(t.feats, nlp::morph::PersonShift);
      if (onePn && p0 && tp && tp != p0 && !(tenses.empty() && moods.size() == 1 && moods[0] == feat::Imperative)) {
        t.feats = setFeat(t.feats, nlp::morph::PersonShift, p0);
        if (n0) t.feats = setFeat(t.feats, nlp::morph::NumberShift, n0 == feat::Pl ? nlp::morph::NumPlur : nlp::morph::NumSing);
      }
      if (!nlp::morph::get(t.feats, nlp::morph::PersonShift) && onePn && p0) {
        t.feats = setFeat(t.feats, nlp::morph::PersonShift, p0);
        if (n0) t.feats = setFeat(t.feats, nlp::morph::NumberShift, n0 == feat::Pl ? nlp::morph::NumPlur : nlp::morph::NumSing);
      }
      if (!vf) t.feats = setFeat(t.feats, nlp::morph::VerbFormShift, nlp::morph::VfFin);
      // "El gatito duerme.": after its noun subject a form that is also an imperative is the indicative
      if (nlp::morph::get(t.feats, nlp::morph::MoodShift) == nlp::morph::MoodImp && i > 0 &&
          (tk[(size_t)i - 1].upos == "NOUN" || tk[(size_t)i - 1].upos == "PROPN" ||
           (tk[(size_t)i - 1].upos == "PRON" && !in(tk[(size_t)i - 1].lower, {"me", "te", "nos", "os", "lo", "la", "le",
                                                                               "los", "las", "les", "se"}))))
        for (const VerbReading& r : vr)
          if (finite(r) && r.mood == feat::Indicative && r.person == 3) {
            t.feats = featsOf(r);
            break;
          }
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
    // an adjective after a degree adverb ("tan extraño") and a noun next to another verb ("Plantamos rosas") stay
    if (i > 0 && in(tk[(size_t)i - 1].lower, {"muy", "tan", "más", "menos", "demasiado", "bastante", "tan"})) continue;
    if (lexOther && anyVerb && t.upos == "NOUN") continue;
    if (i > 0 && in(tk[(size_t)i - 1].lower, {"está", "estás", "estoy", "estamos", "están", "estaba", "estaban", "es",
                                               "son", "era", "eran", "soy", "eres", "somos"}))
      continue;   // "está roto", "es pequeño": a predicate adjective or participle
    if (t.upos == "ADJ") {
      std::vector<lex::Analysis> an;
      lex.lookup(text::es_key(t.lower), an);
      bool adj = false;
      for (const lex::Analysis& a : an) adj = adj || lex.lemma(a.lemma).pos == feat::Adj;
      if (adj) continue;   // "un hoyo profundo"
    }
    const bool doIt = p12 || (imp && (initial || t.upos == "ADJ" || t.upos == "INTJ" || !lexOther)) ||
                      (initial && t.upos == "PROPN" && (imp || fin3)) || (!anyVerb && fin3 && !lexOther);
    if (!doIt) continue;
    // the reading: an imperative at the start of the sentence, else a 1st/2nd person form, else the first finite
    const VerbReading* best = nullptr;
    for (const VerbReading& r : vr)
      if (finite(r) && r.mood == feat::Imperative && initial) { best = &r; break; }
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

// C34: an infinitive, also with an enclitic the tokenizer left on it ("lavarse", "decirle")
bool infinitiveForm(const Token& t) {
  if (fget(t, nlp::morph::VerbFormShift) == nlp::morph::VfInf) return true;
  std::string w = t.lower;
  for (const char* cl : {"selo", "sela", "melo", "telo", "se", "me", "te", "nos", "lo", "la", "le", "los", "las", "les"}) {
    const size_t k = std::strlen(cl);
    if (w.size() > k + 2 && w.compare(w.size() - k, k, cl) == 0) { w.resize(w.size() - k); break; }
  }
  return w.size() > 2 && w[w.size() - 1] == 'r' && (w[w.size() - 2] == 'a' || w[w.size() - 2] == 'e' || w[w.size() - 2] == 'i');
}

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
  // ---- a punctuation root ("¡Que venga el maestro!" parsed under "¡"): the first verb under it is the root -------
  {
    const int r = T.root();
    if (r >= 0 && punct(tk[(size_t)r])) {
      int v = -1;
      for (int k = 0; k < n && v < 0; ++k)
        if (T.verb(k) && T.head(k) == r) v = k;
      if (v >= 0) {
        T.setHead(v, -1);
        tk[(size_t)v].deprel = "root";
        T.rehangChildren(r, v, v);
        T.setHead(r, v);
        tk[(size_t)r].deprel = "punct";
      }
    }
  }
  // ---- C34: a trailing word of address after a comma ("Te lo juro, mamá.", "¿Te gustó, Pablito?", "¿Dónde estabas,
  // hija?"): a vocative of its own (its own root: the unit builder makes it the address), never an argument of the
  // clause; when the parser had made it one, the reading is rebuilt (doubt "addressee-guess": Check) -------------------------
  {
    int last = n - 1;
    while (last >= 0 && punct(tk[(size_t)last])) --last;
    int comma = -1;
    for (int k = last; k > 0 && comma < 0; --k)
      if (tk[(size_t)k].text == ",") comma = k;
      else if (punct(tk[(size_t)k]) && tk[(size_t)k].text != "\xC2\xBF" && tk[(size_t)k].text != "\xC2\xA1") break;
    bool verbBefore = false;
    for (int k = 0; k < comma; ++k) verbBefore = verbBefore || T.verb(k);
    if (comma > 0 && last > comma && last - comma <= 3 && verbBefore) {
      const int a = comma + 1;
      bool span = true, poss = false;
      for (int k = a; k < last && span; ++k) {
        if (in(tk[(size_t)k].lower, {"mi", "mis"})) poss = true;
        else if (!in(tk[(size_t)k].lower, {"querido", "querida", "queridos", "queridas", "pobre"})) span = false;
      }
      const Token& h = tk[(size_t)last];
      const std::string hl = text::lower(h.lemma.empty() ? h.lower : h.lemma);
      const bool address = h.upos == "PROPN" || addressNoun(h.lower) || addressNoun(hl) ||
                           (poss && h.upos == "NOUN") || in(h.lower, {"vida", "amor", "cielo", "corazón"});
      // "Vi a Pedro, mi amigo.": a possessed noun right after a noun is an apposition
      const bool appos = poss && (tk[(size_t)comma - 1].upos == "NOUN" || tk[(size_t)comma - 1].upos == "PROPN");
      bool inner = false;   // the span is no clause of its own (no verb in it)
      for (int k = a; k <= last; ++k) inner = inner || T.verb(k);
      if (span && address && !appos && !inner && (h.upos == "NOUN" || h.upos == "PROPN")) {
        const std::string was = h.deprel;
        const int oldHead = T.head(last);
        if (oldHead < 0) {   // the address was the root: the verb before the comma takes its place
          int v = -1;
          for (int k = 0; k < comma; ++k)
            if (T.verb(k) && (v < 0 || T.head(k) == last)) v = k;
          if (v >= 0) {
            T.setHead(v, -1);
            tk[(size_t)v].deprel = "root";
            for (int k = 0; k < comma + 1; ++k)
              if (k != v && T.head(k) == last) T.setHead(k, v);
          }
        } else {
          for (int k = 0; k <= comma; ++k)
            if (T.head(k) == last) T.setHead(k, oldHead);
        }
        T.setHead(last, -1);
        tk[(size_t)last].deprel = "root";
        for (int k = a; k < last; ++k) {
          T.setHead(k, last);
          tk[(size_t)k].deprel = "det";
        }
        if (in(was, {"nsubj", "obj", "iobj", "obl", "nmod", "conj", "appos", "amod", "xcomp", "ccomp", "advcl"}))
          s.doubt("addressee-guess");   // a flag both engines rate Check
      }
    }
  }
  // ---- C34: "estás triste", "está enfermo": ser / estar read as a full verb with the adjective as its object: the
  // adjective is the predicate (UD copula: the adjective heads the clause, the verb its "cop") ------------------------
  for (int v = 0; v < n; ++v) {
    Token& vt = tk[(size_t)v];
    if (vt.upos != "VERB" && vt.upos != "AUX") continue;
    const std::string vl = text::lower(vt.lemma);
    if (vl != "estar" && vl != "ser") continue;
    int adj = -1;
    for (int k = v + 1; k < n && adj < 0; ++k)
      if (T.head(k) == v && tk[(size_t)k].upos == "ADJ" && in(tk[(size_t)k].deprel, {"obj", "xcomp", "advmod", "obl", "nsubj", "amod"}))
        adj = k;
    if (adj < 0) continue;
    bool nounBetween = false;   // "es una niña buena": the noun is the predicate, not the adjective
    for (int k = v + 1; k < adj; ++k) nounBetween = nounBetween || tk[(size_t)k].upos == "NOUN" || tk[(size_t)k].upos == "PROPN";
    if (nounBetween) continue;
    const int up = T.head(v);
    T.setHead(adj, up);
    tk[(size_t)adj].deprel = vt.deprel;
    for (int k = 0; k < n; ++k)
      if (k != adj && T.head(k) == v) T.setHead(k, adj);
    T.setHead(v, adj);
    vt.deprel = "cop";
    vt.upos = "AUX";
  }
  // ---- C34: "está escondido", "está dormido": estar + a participle that states_es_la.tsv lists as a state is the
  // copula and that adjective (the state, not the resultant passive) ------------------------------------------------
  for (int k = 0; k < n; ++k) {
    Token& t = tk[(size_t)k];
    if (t.upos != "VERB" || fget(t, nlp::morph::VerbFormShift) != nlp::morph::VfPart) continue;
    if (!cd.state(mascSingular(t.lower))) continue;
    for (int q = 0; q < k; ++q)
      if (T.head(q) == k && tk[(size_t)q].deprel == "aux" && text::lower(tk[(size_t)q].lemma) == "estar") {
        tk[(size_t)q].deprel = "cop";
        t.upos = "ADJ";
        t.feats = 0;
        t.lemma = mascSingular(t.lower);
      }
  }
  // ---- C34: a noun with its own preposition is never the object ("escondido debajo de la cama": cama is the place) --
  for (int k = 0; k < n; ++k) {
    Token& t = tk[(size_t)k];
    if ((t.upos != "NOUN" && t.upos != "PROPN") || (t.deprel != "obj" && t.deprel != "nsubj")) continue;
    bool prep = false;
    for (int q = 0; q < k; ++q)
      if (T.head(q) == k && tk[(size_t)q].deprel == "case" && tk[(size_t)q].upos == "ADP" &&
          (!multiwordPrep(tk[(size_t)q].lower).empty() ||
           in(tk[(size_t)q].lower, {"en", "sobre", "bajo", "entre", "tras", "hacia", "desde", "hasta"})))
        prep = true;   // a place ("debajo de la cama"); "llorar por su hijo" keeps its object
    if (prep && T.head(k) >= 0) t.deprel = "obl";
  }
  // ---- C34: "tener que" / "hay que" + infinitive: the infinitive is the complement of tener / haber, "que" its marker
  // (the parser may coordinate them: "tienen que lavarse las manos") ------------------------------------------------
  for (int i = 0; i + 2 < n; ++i) {
    const std::string vl = text::lower(tk[(size_t)i].lemma);
    if ((vl != "tener" && vl != "haber") || !T.verb(i) || tk[(size_t)i + 1].lower != "que") continue;
    const int x = i + 2;
    if (!T.verb(x) || !infinitiveForm(tk[(size_t)x])) continue;
    if (T.head(x) != i) {
      for (int k = 0; k < n; ++k)
        if (T.head(k) == x && tk[(size_t)k].deprel == "cc") { T.setHead(k, i); }
      T.setHead(x, i);
    }
    tk[(size_t)x].deprel = "xcomp";
    T.setHead(i + 1, x);
    tk[(size_t)i + 1].deprel = "mark";
  }
  // ---- C34: a sentence-initial coordinator the parser hung as an adverb ("Pero Pablo seguía hablando.", "Pero la
  // tortuguita no tenía miedo."): the clause's connector (sed) ------------------------------------------------------
  for (int i = 0; i < n; ++i) {
    if (punct(tk[(size_t)i])) continue;
    Token& t = tk[(size_t)i];
    if (in(t.lower, {"pero", "y", "e", "o", "u", "mas"}) && t.upos == "CCONJ" && t.deprel != "cc" && T.verb(T.head(i)))
      t.deprel = "cc";
    break;
  }
  // ---- "por qué" -> why, "a dónde" -> whither ------------------------------------------------------------------------
  for (int i = 0; i + 1 < n; ++i) {
    Token& a = tk[(size_t)i];
    Token& b = tk[(size_t)i + 1];
    if (a.lower == "por" && b.lower == "qué") {
      int r = T.root();
      // C34: the root of this word's own tree (a segment parsed apart: "Mateo, ¿por qué estás triste?")
      for (int x = i + 1, steps = 0; x >= 0 && steps <= n; ++steps) {
        if (T.head(x) < 0) { r = x; break; }
        x = T.head(x);
      }
      if (r == i || r == i + 1) {
        int v = -1;
        for (int k = i + 2; k < n && v < 0; ++k)
          if (T.verb(k) && T.head(k) < 0) v = k;
        if (v >= 0) r = v;
      }
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
    if (multiwordPrep(tk[(size_t)i].lower).empty() ||
        !(tk[(size_t)i + 1].lower == "de" || (tk[(size_t)i + 1].lower == "a" && in(tk[(size_t)i].lower, {"junto", "frente"}))))
      continue;   // C34: "junto a", "frente a" too
    int j = -1;
    for (int k = i + 2; k < n; ++k) {
      const std::string& u = tk[(size_t)k].upos;
      if (u == "NOUN" || u == "PROPN" || u == "PRON" || u == "NUM") { j = k; break; }
      if (u != "DET" && u != "ADJ") break;
    }
    if (j < 0 || tk[(size_t)j].lower == "que") continue;   // "antes de que llegue" is a subordinator
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
  // ---- "antes de que llegue" / "después de que": the adverb is the subordinator of the verb after "que" --------
  for (int i = 0; i + 3 < n; ++i) {
    if (!in(tk[(size_t)i].lower, {"antes", "después", "hasta"}) || tk[(size_t)i + 1].lower != "de" ||
        tk[(size_t)i + 2].lower != "que")
      continue;
    int v = -1;
    for (int k = i + 3; k < n && v < 0; ++k)
      if (T.verb(k)) v = k;
    if (v < 0) continue;
    int top = T.head(i);
    if (top == v) top = T.head(v);
    if (T.head(v) == i || T.head(v) == i + 1 || T.head(v) == i + 2) {
      T.setHead(v, top);
      tk[(size_t)v].deprel = top < 0 ? "root" : "advcl";
    }
    T.rehangChildren(i, v, -1);
    for (int k : {i, i + 1, i + 2}) {
      if (k == v) continue;
      T.setHead(k, v);
      tk[(size_t)k].deprel = "mark";
      tk[(size_t)k].upos = k == i ? "SCONJ" : tk[(size_t)k].upos;
    }
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
    const curated::VerbPrepEntry* vpe = cd.verbPrep(vl, "to");
    if (!person || motionVerb(vl) || dativeVerb(vl) || (vpe && vpe->frame != "obj")) continue;
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
