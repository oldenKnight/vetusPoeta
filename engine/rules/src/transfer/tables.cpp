// Closed-class tables (see tables.h). Lists are our own; Latin headwords carry macrons and are resolved through the
// lexicon (morph::findLemma), so a word missing from the lexicon simply does not apply.
#include "tables.h"

#include <cstring>
#include <utility>

namespace vp::transfer::tables {
namespace {

bool in(const std::string& w, std::initializer_list<const char*> set) {
  for (const char* s : set)
    if (w == s) return true;
  return false;
}

template <size_t N>
const char* lookup(const std::pair<const char*, const char*> (&t)[N], const std::string& w) {
  for (const auto& e : t)
    if (w == e.first) return e.second;
  return nullptr;
}

}  // namespace

bool animateNoun(const std::string& l) {
  return in(l, {"man", "woman", "boy", "girl", "child", "kid", "baby", "person", "people", "friend", "mother",
                "father", "brother", "sister", "son", "daughter", "wife", "husband", "king", "queen", "prince",
                "princess", "duke", "duchess", "teacher", "student", "pupil", "master", "servant", "slave", "soldier",
                "farmer", "doctor", "sailor", "guard", "lady", "gentleman", "lord", "sir", "madam", "god", "goddess",
                "cat", "dog", "horse", "rabbit", "bird", "mouse", "wolf", "fish", "animal", "cook", "gardener",
                "uncle", "aunt", "grandmother", "grandfather", "parent", "neighbour", "neighbor", "guest", "stranger",
                "enemy", "everyone", "everybody",
                // C19: more people and animals of children's stories (agent "by" -> ā/ab, "with" -> cum)
                "hunter", "fisherman", "miller", "shepherd", "woodman", "woodcutter", "witch", "wizard", "giant",
                "dwarf", "fairy", "knight", "lion", "bear", "fox", "cow", "sheep", "goat", "pig", "frog", "owl",
                "dragon", "monkey", "crow", "stork", "merchant", "baker", "butcher", "tailor", "smith", "thief",
                "robber", "beggar", "hero", "priest", "nurse", "emperor", "captain", "pirate", "peasant", "cousin",
                "nephew", "niece", "grandson", "granddaughter", "maid", "giantess", "scarecrow",
                "hombre", "mujer", "niño", "niña", "chico", "chica", "persona",
                "amigo", "amiga", "madre", "padre", "hermano", "hermana", "hijo", "hija", "rey", "reina", "maestro",
                "maestra", "profesor", "profesora", "gato", "perro", "caballo", "conejo", "pájaro", "ratón", "señor",
                "señora", "dios"});
}

// C19: what rises and sets ("the sun rose" -> sōl ortus est)
bool celestialNoun(const std::string& l) { return in(l, {"sun", "moon", "star", "stars", "day", "dawn"}); }

// C19: plants that grow in a garden bed ("flower bed" -> ager flōrum)
bool plantNoun(const std::string& l) {
  return in(l, {"flower", "poppy", "rose", "lily", "tulip", "cabbage", "vegetable", "strawberry", "bean", "pea",
                "lettuce", "carrot", "herb", "daisy", "violet", "onion", "turnip", "pumpkin"});
}

bool narrativeNoun(const std::string& l) {
  return in(l, {"story", "tale", "fable", "history", "joke", "news", "legend", "adventure", "dream", "cuento",
                "historia", "fábula"});
}

bool personalPronoun(const std::string& w) {
  return in(w, {"i", "me", "myself", "you", "yourself", "yourselves", "he", "him", "himself", "she", "her", "herself",
                "it", "itself", "we", "us", "ourselves", "they", "them", "themselves", "thee", "thou", "ye", "yo", "mí",
                "conmigo", "tú", "tu", "te", "ti", "contigo", "vos", "usted", "ustedes", "él", "ella", "ello", "lo",
                "la", "le", "nosotros", "nosotras", "nos", "vosotros", "vosotras", "os", "ellos", "ellas", "los",
                "las", "les", "se", "sí", "consigo"});
}

const char* adverb(const std::string& l, bool motion) {
  if (l == "here") return motion ? "hūc" : "hīc";
  if (l == "there") return motion ? "illūc" : "ibi";
  if (l == "inside" || l == "in") return motion ? "intrō" : "intus";
  if (l == "home") return motion ? "domum" : "domī";
  static const std::pair<const char*, const char*> kAdv[] = {
      {"now", "nunc"},         {"always", "semper"},       {"never", "numquam"},      {"before", "anteā"},
      {"today", "hodiē"},      {"tomorrow", "crās"},       {"yesterday", "herī"},      {"tonight", "hodiē"},
      {"sometimes", "aliquandō"}, {"again", "iterum"},     {"also", "quoque"},        {"already", "iam"},
      {"still", "adhūc"},      {"soon", "mox"},            {"outside", "forīs"},      {"well", "bene"},
      {"much", "multum"},      {"little", "paulum"},       {"first", "prīmum"},       {"then", "tum"},
      {"only", "modo"},        {"very", "valdē"},          {"too", "nimis"},          {"so", "tam"},
      {"often", "saepe"},      {"perhaps", "fortasse"},    {"maybe", "fortasse"},     {"really", "vērō"},
      {"away", "procul"},      {"back", "retrō"},          {"quickly", "celeriter"},  {"slowly", "lentē"},
      {"together", "ūnā"},     {"almost", "paene"},        {"enough", "satis"},       {"immediately", "statim"},
      {"later", "posteā"},     {"once", "semel"},          {"gladly", "libenter"},    {"badly", "male"},
      {"everywhere", "ubīque"}, {"nowhere", "nusquam"},    {"far", "procul"},         {"near", "prope"},
      {"yes", "ita"},          {"no", "nōn"},              {"certainly", "certē"},    {"quite", "satis"},
      {"rather", "potius"},    {"even", "etiam"},     {"as", "tam"},     {"all", "omnīnō"},     {"hereafter", "posthāc"},
      {"besides", "praetereā"}, {"however", "tamen"},  {"exactly", "plānē"},          {"ever", "umquam"},        {"long", "diū"},
      {"loudly", "magnā vōce"}, {"truly", "vērē"},         {"together", "ūnā"},       {"up", "sursum"},
      {"down", "deorsum"},     {"forward", "porrō"},       {"why", "cūr"},            {"how", "quōmodo"},
      {"where", "ubi"},        {"when", "quandō"},        {"at last", "tandem"},     {"suddenly", "subitō"},
      {"late", "sērō"},        {"early", "māne"},          {"twice", "bis"},          {"thrice", "ter"}};   // C20: twice
  const char* r = lookup(kAdv, l);
  if (r && std::strchr(r, ' ')) return nullptr;   // multi-word Latin: not an adverb lemma
  return r;
}

bool dropAdverb(const std::string& l) { return in(l, {"just", "actually", "anyway", "right", "please", "also-"}); }

std::string spanishAdverb(const std::string& l) {
  static const std::pair<const char*, const char*> kEs[] = {
      {"aquí", "here"},       {"acá", "here"},        {"allí", "there"},      {"allá", "there"},   {"ahí", "there"},
      {"ahora", "now"},       {"siempre", "always"},  {"nunca", "never"},     {"jamás", "never"},  {"antes", "before"},
      {"hoy", "today"},       {"mañana", "tomorrow"}, {"ayer", "yesterday"},  {"también", "also"}, {"ya", "already"},
      {"todavía", "still"},   {"aún", "still"},       {"pronto", "soon"},     {"dentro", "inside"}, {"fuera", "outside"},
      {"bien", "well"},       {"mal", "badly"},       {"mucho", "much"},      {"poco", "little"},  {"primero", "first"},
      {"después", "then"},    {"luego", "then"},      {"solo", "only"},       {"sólo", "only"},    {"muy", "very"},
      {"demasiado", "too"},   {"tan", "so"},          {"quizás", "perhaps"},  {"quizá", "perhaps"}, {"rápido", "quickly"},
      {"despacio", "slowly"}, {"juntos", "together"}, {"casi", "almost"},     {"bastante", "enough"}, {"sí", "yes"},
      {"no", "no"},           {"lejos", "far"},       {"cerca", "near"},      {"otra vez", "again"},
      {"a veces", "sometimes"}, {"de nuevo", "again"}, {"a menudo", "often"}, {"en seguida", "immediately"},
      {"enseguida", "immediately"}, {"tal vez", "perhaps"}, {"de verdad", "really"}, {"por fin", "at last"},
      {"claro", "certainly"}, {"adónde", "where"}, {"dónde", "where"}, {"cómo", "how"}, {"cuándo", "when"},
      {"por qué", "why"},     {"acá", "here"},        {"despacio", "slowly"}, {"juntas", "together"},
      {"temprano", "early"},  {"tarde", "late"},      {"de repente", "suddenly"}, {"sin embargo", "however"}};
  if (const char* r = lookup(kEs, l)) return r;
  return l;
}

const char* weekday(const std::string& w) {
  static const std::pair<const char*, const char*> kDays[] = {
      {"monday", "Lūna"},    {"tuesday", "Mārs"},   {"wednesday", "Mercurius"}, {"thursday", "Iuppiter"},
      {"friday", "Venus"},   {"saturday", "Sāturnus"}, {"sunday", "Sōl"},
      {"lunes", "Lūna"},     {"martes", "Mārs"},    {"miércoles", "Mercurius"}, {"jueves", "Iuppiter"},
      {"viernes", "Venus"},  {"sábado", "Sāturnus"}, {"domingo", "Sōl"}};
  return lookup(kDays, w);
}

const char* languageAdverb(const std::string& w) {
  static const std::pair<const char*, const char*> kLang[] = {
      {"latin", "Latīnē"},   {"english", "Anglicē"}, {"greek", "Graecē"},   {"spanish", "Hispānicē"},
      {"french", "Gallicē"}, {"german", "Germānicē"}, {"italian", "Ītalicē"}, {"latín", "Latīnē"},
      {"inglés", "Anglicē"}, {"griego", "Graecē"},    {"español", "Hispānicē"}, {"francés", "Gallicē"}};
  return lookup(kLang, w);
}

bool timeNoun(const std::string& l) {
  return in(l, {"morning", "evening", "night", "day", "week", "month", "year", "hour", "minute", "time", "moment",
                "today", "tomorrow", "yesterday", "tonight", "summer", "winter", "spring", "autumn", "afternoon",
                "weekend", "noon", "midnight", "mañana", "tarde", "noche", "día", "semana", "mes", "año", "hora",
                "momento", "hoy", "ayer", "verano", "invierno"});
}

bool motionVerb(const std::string& l) {
  return in(l, {"go", "come", "run", "walk", "fly", "travel", "hurry", "return", "fall", "jump", "move", "swim",
                "drive", "ride", "climb", "flee", "escape", "send", "lead", "bring", "carry", "throw", "put", "enter",
                "ir", "venir", "correr", "caminar", "volver", "llegar", "andar", "subir", "bajar", "entrar", "salir",
                "regresar", "caer", "huir", "escapar", "viajar", "volar", "nadar", "saltar", "meter", "poner",
                "llevar", "traer", "marchar", "acercar"});
}

bool impersonalAdjective(const std::string& l) {
  return in(l, {"dark", "light", "late", "early", "cold", "hot", "warm", "true", "false", "possible", "impossible",
                "easy", "difficult", "hard", "good", "bad", "important", "necessary", "clear", "strange", "nice",
                "fine", "sad", "funny", "better", "worse", "right", "wrong", "certain", "obvious", "useless",
                "oscuro", "tarde", "temprano", "frío", "caliente", "verdad", "posible", "imposible", "fácil",
                "difícil", "bueno", "malo", "importante", "necesario", "claro", "extraño"});
}

const char* verb(const std::string& l) {
  static const std::pair<const char*, const char*> kV[] = {
      {"be", "sum"}, {"ser", "sum"}, {"estar", "sum"}, {"can", "possum"}, {"poder", "possum"},
      {"have", "habeō"}, {"tener", "habeō"}, {"do", "faciō"}, {"hacer", "faciō"}};
  return lookup(kV, l);
}

bool stateVerb(std::string_view k) {
  static const char* const kState[] = {"sum", "habeo", "scio", "nescio", "habito", "uiuo", "sedeo", "sto", "iaceo",
                                       "puto", "credo", "amo", "possum", "nolo", "cupio", "timeo", "erro", "memini",
                                       "spero", "uideor", "debeo", "frigeo", "careo", "pendeo", "gaudeo", "ualeo",
                                       "esurio", "sitio", "caleo", "odi", "egeo", "soleo", "existimo", "arbitror"};
  for (const char* s : kState)
    if (k == s) return true;
  return false;
}

const char* interjection(const std::string& w) {
  static const std::pair<const char*, const char*> kI[] = {
      {"oh", "ō"},   {"o", "ō"},      {"alas", "ēheu"}, {"hey", "heus"}, {"wow", "papae"}, {"hooray", "iō"},
      {"ugh", "vah"}, {"ah", "ā"},    {"ay", "heu"},    {"eh", "heus"},  {"oye", "heus"},
      // C19
      {"hurrah", "iō"}, {"hurray", "iō"}, {"yippee", "iō"}, {"bravo", "euge"}, {"aha", "ā"}};
  return lookup(kI, w);
}

const char* connector(const std::string& w, bool afterFirst) {
  if (w == "then") return afterFirst ? "deinde" : "igitur";
  static const std::pair<const char*, const char*> kC[] = {
      {"and", "et"},       {"but", "sed"},        {"or", "aut"},          {"so", "itaque"},   {"because", "quia"},
      {"if", "sī"},        {"nor", "neque"},      {"also", "quoque"},     {"yet", "tamen"},   {"therefore", "itaque"},
      {"however", "tamen"}, {"when", "cum"},      {"although", "quamquam"}, {"while", "dum"},  {"unless", "nisi"},
      {"for", "nam"},      {"besides", "praetereā"}, {"moreover", "praetereā"}, {"still", "tamen"}, {"thus", "ita"},
      {"hence", "itaque"}};
  return lookup(kC, w);
}

const char* cardinal(int v) {
  switch (v) {
    case 1: return "ūnus"; case 2: return "duo"; case 3: return "trēs"; case 4: return "quattuor";
    case 5: return "quīnque"; case 6: return "sex"; case 7: return "septem"; case 8: return "octō";
    case 9: return "novem"; case 10: return "decem"; case 11: return "ūndecim"; case 12: return "duodecim";
    case 20: return "vīgintī"; case 30: return "trīgintā"; case 100: return "centum"; case 1000: return "mīlle";
    // C19
    case 13: return "tredecim"; case 14: return "quattuordecim"; case 15: return "quīndecim";
    case 16: return "sēdecim"; case 17: return "septendecim"; case 18: return "duodēvīgintī";
    case 19: return "ūndēvīgintī"; case 40: return "quadrāgintā"; case 50: return "quīnquāgintā";
    case 60: return "sexāgintā"; case 70: return "septuāgintā"; case 80: return "octōgintā"; case 90: return "nōnāgintā";
    default: return nullptr;
  }
}

const char* ordinal(int v) {
  switch (v) {
    case 1: return "prīmus"; case 2: return "secundus"; case 3: return "tertius"; case 4: return "quārtus";
    case 5: return "quīntus"; case 6: return "sextus"; case 7: return "septimus"; case 8: return "octāvus";
    case 9: return "nōnus"; case 10: return "decimus"; case 11: return "ūndecimus"; case 12: return "duodecimus";
    default: return nullptr;
  }
}

}  // namespace vp::transfer::tables
