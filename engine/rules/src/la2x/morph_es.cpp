// Spanish (es-MX) word morphology for the readable sentence: verb conjugation (regular, stem-changing,
// orthographic and the common irregular verbs), noun/adjective number and gender, written accents with clitics.
// "ustedes" (2nd plural) conjugates as the 3rd plural; vosotros forms are never produced.
#include <algorithm>
#include <cstring>

#include "internal.h"
#include "vp/features.h"
#include "vp/text.h"

namespace vp::la2x::detail::es {

namespace {

// index 0..5: yo tú él nosotros (vosotros) ellos; ustedes -> 5
int slot(int person, int number) {
  if (person < 1 || person > 3) person = 3;
  if (number == 2) return person == 1 ? 3 : 5;
  return person - 1;
}

bool endsWith(const std::string& s, const char* x) {
  const size_t n = std::strlen(x);
  return s.size() >= n && s.compare(s.size() - n, n, x) == 0;
}

// ---- prefixes for compound verbs (con-tener, de-volver, pro-poner, a-traer ...) ----
const char* const kPrefixes[] = {"",     "a",    "ab",   "ad",   "atra", "com",  "con",  "contra", "de",  "des",
                                 "dis",  "en",   "entre", "ex",  "im",   "man",  "ob",   "pos",    "pre", "pro",
                                 "re",   "retro", "satis", "sobre", "sos", "su",  "sus",  "tras"};

// The base verb of a compound when `inf` = prefix + base (base must be in `list`). "" when none.
std::string baseOf(const std::string& inf, const char* const* list, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const std::string b = list[i];
    if (!endsWith(inf, b.c_str())) continue;
    const std::string pre = inf.substr(0, inf.size() - b.size());
    for (const char* p : kPrefixes)
      if (pre == p) return b;
  }
  return std::string();
}

struct Strong {   // irregular verbs: data per base
  const char* inf;
  const char* yo;        // present 1sg ("" regular)
  const char* pret;      // strong preterite stem ("" regular)
  const char* fut;       // future/conditional stem ("" regular)
  const char* imp;       // tú imperative ("" regular)
  const char* part;      // past participle ("" regular)
  const char* subj;      // present subjunctive stem ("" from yo)
};
const Strong kStrong[] = {
    {"caber", "quepo", "cup", "cabr", "", "", ""},
    {"caer", "caigo", "", "", "", "caído", ""},
    {"decir", "digo", "dij", "dir", "di", "dicho", ""},
    {"hacer", "hago", "hic", "har", "haz", "hecho", ""},
    {"oír", "oigo", "", "", "", "oído", ""},
    {"poder", "", "pud", "podr", "", "", ""},
    {"poner", "pongo", "pus", "pondr", "pon", "puesto", ""},
    {"querer", "", "quis", "querr", "", "", ""},
    {"saber", "sé", "sup", "sabr", "", "", "sep"},
    {"salir", "salgo", "", "saldr", "sal", "", ""},
    {"tener", "tengo", "tuv", "tendr", "ten", "", ""},
    {"traer", "traigo", "traj", "", "", "traído", ""},
    {"valer", "valgo", "", "valdr", "", "", ""},
    {"venir", "vengo", "vin", "vendr", "ven", "", ""},
    {"andar", "", "anduv", "", "", "", ""},
    {"conducir", "conduzco", "conduj", "", "", "", ""},
    {"producir", "produzco", "produj", "", "", "", ""},
    {"traducir", "traduzco", "traduj", "", "", "", ""},
    {"leer", "", "", "", "", "leído", ""},
    {"creer", "", "", "", "", "creído", ""},
    {"reír", "río", "", "", "ríe", "reído", ""},
    {"escribir", "", "", "", "", "escrito", ""},
    {"abrir", "", "", "", "", "abierto", ""},
    {"cubrir", "", "", "", "", "cubierto", ""},
    {"morir", "", "", "", "", "muerto", ""},
    {"romper", "", "", "", "", "roto", ""},
    {"volver", "", "", "", "", "vuelto", ""},
    {"resolver", "", "", "", "", "resuelto", ""},
    {"freír", "", "", "", "", "frito", ""},
    {"imprimir", "", "", "", "", "impreso", ""},
};

const Strong* strong(const std::string& inf, std::string& prefix) {
  static std::vector<const char*> names;
  if (names.empty())
    for (const Strong& s : kStrong) names.push_back(s.inf);
  const std::string b = baseOf(inf, names.data(), names.size());
  if (b.empty()) return nullptr;
  prefix = inf.substr(0, inf.size() - b.size());
  for (const Strong& s : kStrong)
    if (b == s.inf) return &s;
  return nullptr;
}

// stem changes: 'e' (e->ie), 'o' (o->ue), 'i' (e->i), 'E' (e->ie, -ir: e->i in 3rd preterite), 'O' (o->ue, -ir: o->u),
// 'u' (u->ue)
const char* const kIe[] = {"acertar", "apretar", "ascender", "atravesar", "calentar", "cerrar", "comenzar", "confesar",
                           "defender", "descender", "despertar", "empezar", "encender", "entender", "gobernar",
                           "merendar", "negar", "nevar", "pensar", "perder", "recomendar", "regar", "sentar",
                           "temblar", "tender", "querer"};
const char* const kIeI[] = {"advertir", "arrepentir", "consentir", "convertir", "divertir", "herir", "hervir",
                            "invertir", "mentir", "preferir", "referir", "requerir", "sentir", "sugerir"};
const char* const kUe[] = {"acordar", "acostar", "almorzar", "aprobar", "colgar", "consolar", "contar", "costar",
                           "demostrar", "devolver", "doler", "encontrar", "forzar", "llover", "morder", "mostrar",
                           "mover", "poder", "probar", "recordar", "resolver", "rogar", "soler", "sonar", "soñar",
                           "torcer", "tronar", "volar", "volver", "renovar", "oler"};
const char* const kUeU[] = {"dormir", "morir"};
const char* const kI[] = {"competir", "conseguir", "corregir", "despedir", "elegir", "freír", "gemir", "impedir",
                          "medir", "pedir", "perseguir", "reír", "rendir", "repetir", "seguir", "servir", "sonreír",
                          "vestir", "reñir", "teñir"};

char stemType(const std::string& inf) {
  if (inf == "jugar") return 'u';
  if (!baseOf(inf, kIeI, sizeof(kIeI) / sizeof(*kIeI)).empty()) return 'E';
  if (!baseOf(inf, kUeU, sizeof(kUeU) / sizeof(*kUeU)).empty()) return 'O';
  if (!baseOf(inf, kI, sizeof(kI) / sizeof(*kI)).empty()) return 'i';
  if (!baseOf(inf, kIe, sizeof(kIe) / sizeof(*kIe)).empty()) return 'e';
  if (!baseOf(inf, kUe, sizeof(kUe) / sizeof(*kUe)).empty()) return 'o';
  return 0;
}

// Replaces the last occurrence of the vowel in the stem.
std::string changeStem(const std::string& stem, char type, bool strongChange) {
  // strongChange: present 1-3 sg + 3 pl (ie/ue/i); weak: -ir preterite 3rd / gerund / subj 1-2 pl (i/u)
  std::string s = stem;
  size_t at = std::string::npos;
  const char want = (type == 'o' || type == 'O') ? 'o' : type == 'u' ? 'u' : 'e';
  for (size_t i = s.size(); i-- > 0;)
    if (s[i] == want) { at = i; break; }
  if (at == std::string::npos) return s;
  if (strongChange) {
    std::string rep = type == 'e' || type == 'E' ? "ie" : type == 'o' || type == 'O' ? "ue" : type == 'u' ? "ue" : "i";
    // word-initial: e -> ye (errar), o -> hue (oler)
    if (at == 0 && rep == "ie") rep = "ye";
    if (at == 0 && rep == "ue") rep = "hue";
    s.replace(at, 1, rep);
  } else {
    if (type == 'E' || type == 'i') s.replace(at, 1, "i");
    else if (type == 'O') s.replace(at, 1, "u");
  }
  return s;
}

// orthography before an ending: keep the sound of the stem's last consonant
std::string ortho(const std::string& stem, const std::string& ending, char cls) {
  if (ending.empty()) return stem + ending;
  const char e0 = ending[0];
  const bool front = e0 == 'e' || ((unsigned char)e0 == 0xC3 && ending.size() > 1 && (unsigned char)ending[1] == 0xA9);   // e / é
  const bool back = e0 == 'a' || e0 == 'o' || ((unsigned char)e0 == 0xC3 && ending.size() > 1 &&
                                               ((unsigned char)ending[1] == 0xA1 || (unsigned char)ending[1] == 0xB3));
  std::string s = stem;
  if (cls == 'a' && front) {
    if (endsWith(s, "c")) s = s.substr(0, s.size() - 1) + "qu";
    else if (endsWith(s, "gu")) s = s.substr(0, s.size() - 2) + "gü";
    else if (endsWith(s, "g")) s += "u";
    else if (endsWith(s, "z")) s = s.substr(0, s.size() - 1) + "c";
  } else if (cls != 'a' && back) {
    if (endsWith(s, "gu")) s = s.substr(0, s.size() - 1);   // seguir -> sigo
    else if (endsWith(s, "g")) s = s.substr(0, s.size() - 1) + "j";   // coger -> cojo
    else if (endsWith(s, "c")) {
      const std::string pre = s.substr(0, s.size() - 1);
      const bool vowelBefore = !pre.empty() && std::strchr("aeiouáéíóú", pre.back()) != nullptr;
      s = vowelBefore ? pre + "zc" : pre + "z";   // conocer -> conozco, vencer -> venzo
    }
  }
  return s + ending;
}

std::string regular(const std::string& inf, VTense t, int idx) {
  if (inf.size() < 3) return inf;
  std::string cls3 = inf.substr(inf.size() - 2);
  if (endsWith(inf, "ír")) cls3 = "ir";
  const char cls = cls3 == "ar" ? 'a' : cls3 == "er" ? 'e' : 'i';
  const std::string stem = endsWith(inf, "ír") ? inf.substr(0, inf.size() - 3) : inf.substr(0, inf.size() - 2);
  const char type = stemType(inf);
  const bool uir = cls == 'i' && endsWith(stem, "u") && !endsWith(stem, "gu") && !endsWith(stem, "qu");
  const bool vowelStem = !stem.empty() && std::strchr("aeo", stem.back()) != nullptr && cls != 'a';   // leer, caer, oír
  static const char* const kPresA[] = {"o", "as", "a", "amos", "áis", "an"};
  static const char* const kPresE[] = {"o", "es", "e", "emos", "éis", "en"};
  static const char* const kPresI[] = {"o", "es", "e", "imos", "ís", "en"};
  static const char* const kImpA[] = {"aba", "abas", "aba", "ábamos", "abais", "aban"};
  static const char* const kImpE[] = {"ía", "ías", "ía", "íamos", "íais", "ían"};
  static const char* const kPretA[] = {"é", "aste", "ó", "amos", "asteis", "aron"};
  static const char* const kPretE[] = {"í", "iste", "ió", "imos", "isteis", "ieron"};
  static const char* const kSubjA[] = {"e", "es", "e", "emos", "éis", "en"};
  static const char* const kSubjE[] = {"a", "as", "a", "amos", "áis", "an"};
  static const char* const kFut[] = {"é", "ás", "á", "emos", "éis", "án"};
  static const char* const kCond[] = {"ía", "ías", "ía", "íamos", "íais", "ían"};
  const bool strongSlot = idx == 0 || idx == 1 || idx == 2 || idx == 5;
  switch (t) {
    case VTense::Infinitive: return inf;
    case VTense::Present: {
      const char* e = (cls == 'a' ? kPresA : cls == 'e' ? kPresE : kPresI)[idx];
      std::string st = type && strongSlot ? changeStem(stem, type, true) : stem;
      if (uir && std::string(e).compare(0, 1, "i") != 0 && std::string(e).compare(0, 1, "í") != 0) st += "y";
      return ortho(st, e, cls);
    }
    case VTense::SubjPresent: {
      const char* e = (cls == 'a' ? kSubjA : kSubjE)[idx];
      std::string st = stem;
      if (type && strongSlot) st = changeStem(stem, type, true);
      else if (type && cls == 'i') st = changeStem(stem, type, false);
      if (uir) st += "y";
      return ortho(st, e, cls);
    }
    case VTense::Imperfect: return stem + (cls == 'a' ? kImpA : kImpE)[idx];
    case VTense::Preterite: {
      if (cls == 'a') return ortho(stem, kPretA[idx], 'a');
      std::string st = stem;
      if (type && cls == 'i' && (idx == 2 || idx == 5)) st = changeStem(stem, type, false);
      if ((vowelStem || uir) && (idx == 2 || idx == 5)) return st + (idx == 2 ? "yó" : "yeron");
      if (vowelStem && idx != 2 && idx != 5) {
        static const char* const kAcc[] = {"í", "íste", "", "ímos", "ísteis", ""};
        return st + kAcc[idx];
      }
      return st + kPretE[idx];
    }
    case VTense::SubjImperfect: {
      // from the 3rd plural preterite: -ron -> -ra
      std::string p3 = regular(inf, VTense::Preterite, 5);
      std::string base = p3.substr(0, p3.size() - 3);
      static const char* const kEnd[] = {"ra", "ras", "ra", "ramos", "rais", "ran"};
      if (idx == 3) {
        // accent on the vowel before -ramos: amáramos, comiéramos
        if (endsWith(base, "a")) base = base.substr(0, base.size() - 1) + "á";
        else if (endsWith(base, "e")) base = base.substr(0, base.size() - 1) + "é";
      }
      return base + kEnd[idx];
    }
    case VTense::Future: return inf + kFut[idx];
    case VTense::Conditional: return inf + kCond[idx];
    case VTense::Imperative: {
      if (idx == 1) return regular(inf, VTense::Present, 2);
      return regular(inf, VTense::SubjPresent, 5);
    }
    case VTense::PastPart: {
      if (cls == 'a') return stem + "ado";
      if (vowelStem) return stem + "ído";
      return stem + "ido";
    }
    case VTense::Gerund: {
      if (cls == 'a') return stem + "ando";
      std::string st = type && cls == 'i' ? changeStem(stem, type, false) : stem;
      if (vowelStem || uir) return st + "yendo";
      return st + "iendo";
    }
  }
  return inf;
}

std::string full(const std::string& inf, VTense t, int idx) {
  // fully irregular verbs
  struct Full { const char* inf; const char* pres[6]; const char* imp[6]; const char* pret[6]; const char* subj[6];
                const char* impv; const char* part; const char* ger; };
  static const Full kFull[] = {
      {"ser", {"soy", "eres", "es", "somos", "sois", "son"}, {"era", "eras", "era", "éramos", "erais", "eran"},
       {"fui", "fuiste", "fue", "fuimos", "fuisteis", "fueron"}, {"sea", "seas", "sea", "seamos", "seáis", "sean"},
       "sé", "sido", "siendo"},
      {"ir", {"voy", "vas", "va", "vamos", "vais", "van"}, {"iba", "ibas", "iba", "íbamos", "ibais", "iban"},
       {"fui", "fuiste", "fue", "fuimos", "fuisteis", "fueron"}, {"vaya", "vayas", "vaya", "vayamos", "vayáis", "vayan"},
       "ve", "ido", "yendo"},
      {"estar", {"estoy", "estás", "está", "estamos", "estáis", "están"},
       {"estaba", "estabas", "estaba", "estábamos", "estabais", "estaban"},
       {"estuve", "estuviste", "estuvo", "estuvimos", "estuvisteis", "estuvieron"},
       {"esté", "estés", "esté", "estemos", "estéis", "estén"}, "está", "estado", "estando"},
      {"haber", {"he", "has", "ha", "hemos", "habéis", "han"}, {"había", "habías", "había", "habíamos", "habíais", "habían"},
       {"hube", "hubiste", "hubo", "hubimos", "hubisteis", "hubieron"}, {"haya", "hayas", "haya", "hayamos", "hayáis", "hayan"},
       "he", "habido", "habiendo"},
      {"dar", {"doy", "das", "da", "damos", "dais", "dan"}, {"daba", "dabas", "daba", "dábamos", "dabais", "daban"},
       {"di", "diste", "dio", "dimos", "disteis", "dieron"}, {"dé", "des", "dé", "demos", "deis", "den"}, "da", "dado",
       "dando"},
      {"ver", {"veo", "ves", "ve", "vemos", "veis", "ven"}, {"veía", "veías", "veía", "veíamos", "veíais", "veían"},
       {"vi", "viste", "vio", "vimos", "visteis", "vieron"}, {"vea", "veas", "vea", "veamos", "veáis", "vean"}, "ve",
       "visto", "viendo"},
  };
  for (const Full& f : kFull) {
    if (inf != f.inf) continue;
    switch (t) {
      case VTense::Present: return f.pres[idx];
      case VTense::Imperfect: return f.imp[idx];
      case VTense::Preterite: return f.pret[idx];
      case VTense::SubjPresent: return f.subj[idx];
      case VTense::SubjImperfect: {
        std::string base = std::string(f.pret[5]);
        base = base.substr(0, base.size() - 3);
        static const char* const kEnd[] = {"ra", "ras", "ra", "ramos", "rais", "ran"};
        if (idx == 3) {
          if (endsWith(base, "e")) base = base.substr(0, base.size() - 1) + "é";
          else if (endsWith(base, "a")) base = base.substr(0, base.size() - 1) + "á";
        }
        return base + kEnd[idx];
      }
      case VTense::Imperative: return idx == 1 ? std::string(f.impv) : std::string(f.subj[5]);
      case VTense::PastPart: return f.part;
      case VTense::Gerund: return f.ger;
      case VTense::Future: return std::string(f.inf) + std::vector<const char*>{"é", "ás", "á", "emos", "éis", "án"}[(size_t)idx];
      case VTense::Conditional: return std::string(f.inf) + std::vector<const char*>{"ía", "ías", "ía", "íamos", "íais", "ían"}[(size_t)idx];
      case VTense::Infinitive: return inf;
    }
  }
  return std::string();
}

std::string conj(const std::string& inf, VTense t, int idx) {
  std::string f = full(inf, t, idx);
  if (!f.empty()) return f;
  std::string prefix;
  const Strong* s = strong(inf, prefix);
  if (!s) return regular(inf, t, idx);
  const std::string base = s->inf;
  const char cls = endsWith(base, "ar") ? 'a' : endsWith(base, "er") ? 'e' : 'i';
  const std::string yo = s->yo;
  const std::string subjStem = *s->subj ? std::string(s->subj) : (!yo.empty() && endsWith(yo, "o") ? yo.substr(0, yo.size() - 1) : std::string());
  switch (t) {
    case VTense::Present: {
      if (idx == 0 && !yo.empty()) return prefix + yo;
      if (base == "decir") { static const char* const k[] = {"digo", "dices", "dice", "decimos", "decís", "dicen"}; return prefix + k[idx]; }
      if (base == "tener") { static const char* const k[] = {"tengo", "tienes", "tiene", "tenemos", "tenéis", "tienen"}; return prefix + k[idx]; }
      if (base == "venir") { static const char* const k[] = {"vengo", "vienes", "viene", "venimos", "venís", "vienen"}; return prefix + k[idx]; }
      if (base == "oír") { static const char* const k[] = {"oigo", "oyes", "oye", "oímos", "oís", "oyen"}; return prefix + k[idx]; }
      if (base == "reír") { static const char* const k[] = {"río", "ríes", "ríe", "reímos", "reís", "ríen"}; return prefix + k[idx]; }
      return prefix + regular(base, t, idx);
    }
    case VTense::SubjPresent: {
      if (!subjStem.empty()) {
        static const char* const kA[] = {"a", "as", "a", "amos", "áis", "an"};
        return prefix + subjStem + kA[idx];
      }
      return prefix + regular(base, t, idx);
    }
    case VTense::Preterite: {
      if (!*s->pret) return prefix + regular(base, t, idx);
      const std::string st = s->pret;
      const bool j = endsWith(st, "j");
      static const char* const kStrongEnd[] = {"e", "iste", "o", "imos", "isteis", "ieron"};
      std::string e = kStrongEnd[idx];
      if (j && idx == 5) e = "eron";
      if (base == "hacer" && idx == 2) return prefix + "hizo";
      return prefix + st + e;
    }
    case VTense::SubjImperfect: {
      if (!*s->pret) return prefix + regular(base, t, idx);
      std::string p3 = conj(inf, VTense::Preterite, 5);
      std::string b = p3.substr(0, p3.size() - 3);
      static const char* const kEnd[] = {"ra", "ras", "ra", "ramos", "rais", "ran"};
      if (idx == 3) {
        if (endsWith(b, "e")) b = b.substr(0, b.size() - 1) + "é";
      }
      return b + kEnd[idx];
    }
    case VTense::Future:
    case VTense::Conditional: {
      if (!*s->fut) return prefix + regular(base, t, idx);
      static const char* const kF[] = {"é", "ás", "á", "emos", "éis", "án"};
      static const char* const kC[] = {"ía", "ías", "ía", "íamos", "íais", "ían"};
      return prefix + s->fut + (t == VTense::Future ? kF[idx] : kC[idx]);
    }
    case VTense::Imperative: {
      if (idx == 1 && *s->imp) {
        std::string im = s->imp;
        if (!prefix.empty() && (base == "tener" || base == "venir" || base == "poner" || base == "hacer")) {
          // compound imperatives carry the accent: contén, convén, propón
          if (im == "ten") im = "tén";
          else if (im == "ven") im = "vén";
          else if (im == "pon") im = "pón";
        }
        return prefix + im;
      }
      if (idx == 1) return conj(inf, VTense::Present, 2);
      return conj(inf, VTense::SubjPresent, 5);
    }
    case VTense::PastPart: return *s->part ? prefix + s->part : prefix + regular(base, t, idx);
    case VTense::Gerund: {
      if (base == "decir") return prefix + "diciendo";
      if (base == "venir") return prefix + "viniendo";
      if (base == "poder") return prefix + "pudiendo";
      return prefix + regular(base, t, idx);
    }
    case VTense::Imperfect:
    case VTense::Infinitive: return prefix + regular(base, t, idx);
  }
  (void)cls;
  return inf;
}

}  // namespace

bool reflexive(std::string_view inf) {
  std::string s(inf);
  const size_t sp = s.find(' ');
  if (sp != std::string::npos) s = s.substr(0, sp);
  return s.size() > 3 && endsWith(s, "se") && (endsWith(s, "arse") || endsWith(s, "erse") || endsWith(s, "irse") || endsWith(s, "írse"));
}

std::string unreflexive(std::string_view inf) {
  std::string s(inf);
  const size_t sp = s.find(' ');
  std::string rest;
  if (sp != std::string::npos) { rest = s.substr(sp); s = s.substr(0, sp); }
  if (reflexive(s)) s = s.substr(0, s.size() - 2);
  return s + rest;
}

std::string verb(std::string_view infinitive, VTense t, int person, int number) {
  std::string s = unreflexive(infinitive);
  std::string rest;
  const size_t sp = s.find(' ');
  if (sp != std::string::npos) { rest = s.substr(sp); s = s.substr(0, sp); }
  int idx = slot(person, number);
  if (t == VTense::Imperative) idx = number == 2 ? 5 : 1;
  return conj(s, t, idx) + rest;
}

std::string plural(std::string_view noun) {
  std::string w(noun);
  if (w.empty()) return w;
  // "casa de campo" -> "casas de campo"
  const size_t de = w.find(" de ");
  if (de != std::string::npos) return plural(w.substr(0, de)) + w.substr(de);
  const size_t sp = w.find(' ');
  if (sp != std::string::npos) return plural(w.substr(0, sp)) + " " + plural(w.substr(sp + 1));
  if (endsWith(w, "z")) return w.substr(0, w.size() - 1) + "ces";
  if (endsWith(w, "s") || endsWith(w, "x")) {
    // lunes, crisis: unchanged when unstressed; mes -> meses, país -> países
    if (endsWith(w, "és")) return w.substr(0, w.size() - 3) + "eses";
    if (endsWith(w, "ís")) return w + "es";
    if (w.size() <= 4) return w + "es";
    return w;
  }
  static const char* const kAcc[][2] = {{"ón", "o"}, {"án", "a"}, {"én", "e"}, {"ín", "i"}, {"ún", "u"}};
  for (const auto& a : kAcc)
    if (endsWith(w, a[0])) return w.substr(0, w.size() - std::strlen(a[0])) + a[1] + "nes";
  const char last = w.back();
  if (std::strchr("aeiou", last)) return w + "s";
  if (endsWith(w, "á") || endsWith(w, "é") || endsWith(w, "ó")) return w + "s";
  if (endsWith(w, "í") || endsWith(w, "ú")) return w + "es";
  if (endsWith(w, "en") && w.size() > 4) {
    // joven -> jóvenes, examen -> exámenes, imagen -> imágenes (accent on the antepenult)
    static const char* const kEn[][2] = {{"joven", "jóvenes"}, {"examen", "exámenes"}, {"imagen", "imágenes"},
                                         {"origen", "orígenes"}, {"orden", "órdenes"}, {"virgen", "vírgenes"}};
    for (const auto& x : kEn)
      if (w == x[0]) return x[1];
  }
  return w + "es";
}

std::string adjective(std::string_view masculine, uint8_t gender, uint8_t number) {
  std::string w(masculine);
  if (w.empty()) return w;
  const size_t sp = w.find(' ');
  if (sp != std::string::npos) {
    // "de esclavo": invariable; "muy bueno": inflect the last word
    if (w.compare(0, 3, "de ") == 0) return w;
    return w.substr(0, sp + 1) + adjective(w.substr(sp + 1), gender, number);
  }
  const bool fem = gender == feat::F;
  std::string g = w;
  if (fem) {
    if (endsWith(w, "o")) g = w.substr(0, w.size() - 1) + "a";
    else if (endsWith(w, "or") && w != "mejor" && w != "peor" && w != "mayor" && w != "menor" && w != "superior" &&
             w != "inferior" && w != "exterior" && w != "interior")
      g = w + "a";
    else if (endsWith(w, "ón")) g = w.substr(0, w.size() - 3) + "ona";
    else if (endsWith(w, "án")) g = w.substr(0, w.size() - 3) + "ana";
    else if (endsWith(w, "és") && w != "cortés") g = w.substr(0, w.size() - 3) + "esa";
    else if (w == "un") g = "una";
    else if (w == "ningún") g = "ninguna";
    else if (w == "algún") g = "alguna";
  }
  if (number != 2) return g;
  if (g == "ningún" || g == "ninguna") return g;
  if (g == "un") return "unos";
  if (g == "algún") return "algunos";
  if (g == "ningún") return "ningunos";
  return plural(g);
}

uint8_t nounGender(std::string_view noun, uint8_t latinGender) {
  std::string w(noun);
  const size_t sp = w.find(' ');
  if (sp != std::string::npos) w = w.substr(0, sp);
  static const char* const kMasc[] = {"día", "mapa", "problema", "planeta", "poeta", "idioma", "clima", "tema",
                                      "sistema", "programa", "drama", "papa", "sofá", "cometa", "profeta", "pirata",
                                      "atleta", "monarca", "patriarca"};
  static const char* const kFem[] = {"mano", "radio", "foto", "moto", "flor", "miel", "sal", "piel", "ley", "luz",
                                     "voz", "cruz", "paz", "nariz", "raíz", "vez", "noche", "nieve", "nave", "llave",
                                     "calle", "carne", "fuente", "frente", "gente", "leche", "mente", "muerte",
                                     "nube", "parte", "sangre", "sede", "suerte", "tarde", "torre", "madre",
                                     "clase", "fe", "hambre", "sed", "red", "pared", "lid", "merced", "ciudad",
                                     "mujer", "imagen", "orden", "señal", "cárcel", "col", "miel", "hiel", "tos",
                                     "res", "mies", "crisis", "tesis", "fiebre", "costumbre", "lumbre", "cumbre",
                                     "serpiente", "liebre", "ave", "hambre"};
  for (const char* x : kMasc)
    if (w == x) return feat::M;
  for (const char* x : kFem)
    if (w == x) return feat::F;
  if (endsWith(w, "a") || endsWith(w, "ción") || endsWith(w, "sión") || endsWith(w, "dad") || endsWith(w, "tad") ||
      endsWith(w, "tud") || endsWith(w, "umbre") || endsWith(w, "ez") || endsWith(w, "triz") || endsWith(w, "isis"))
    return feat::F;
  if (endsWith(w, "e") || endsWith(w, "o") || endsWith(w, "or") || endsWith(w, "aje") || endsWith(w, "ma") || endsWith(w, "án") ||
      endsWith(w, "ón") || endsWith(w, "ín") || endsWith(w, "és") || endsWith(w, "l") || endsWith(w, "r"))
    return feat::M;
  if (latinGender == feat::F || latinGender == feat::FN) return feat::F;
  return feat::M;
}

bool startsWithStressedA(std::string_view noun) {
  std::string w(noun);
  const size_t sp = w.find(' ');
  if (sp != std::string::npos) w = w.substr(0, sp);
  static const char* const k[] = {"agua", "águila", "alma", "arma", "ave", "aula", "área", "hambre", "hacha", "hada",
                                  "ala", "asta", "ancla", "alba", "arca", "aria", "haba", "habla", "hampa", "ama"};
  for (const char* x : k)
    if (w == x) return true;
  return false;
}

std::string withClitics(std::string_view verbForm, std::string_view clitics) {
  std::string v(verbForm);
  if (clitics.empty()) return v;
  std::u32string u = text::toUtf32(v);
  auto isV = [](char32_t c) { return std::u32string(U"aeiouáéíóú").find(c) != std::u32string::npos; };
  auto accented = [](char32_t c) { return std::u32string(U"áéíóú").find(c) != std::u32string::npos; };
  bool hasAccent = false;
  for (char32_t c : u) hasAccent = hasAccent || accented(c);
  // syllable nuclei (a strong vowel or an i/u group counts once)
  std::vector<size_t> nuclei;
  for (size_t i = 0; i < u.size(); ++i) {
    if (!isV(u[i])) continue;
    if (i > 0 && isV(u[i - 1])) {
      // diphthong: weak + strong / strong + weak / weak + weak stay one nucleus
      const bool weakPrev = u[i - 1] == U'i' || u[i - 1] == U'u';
      const bool weakCur = u[i] == U'i' || u[i] == U'u';
      if (weakPrev || weakCur) {
        // keep the strong vowel as the nucleus position
        if (weakPrev && !weakCur) nuclei.back() = i;
        continue;
      }
    }
    nuclei.push_back(i);
  }
  int added = 0;
  {
    std::u32string c = text::toUtf32(std::string(clitics));
    bool in = false;
    for (char32_t ch : c) {
      const bool vv = isV(ch);
      if (vv && !in) ++added;
      in = vv;
    }
  }
  if (hasAccent) {
    // monosyllables with an accent lose it ("dé" + "me" -> "deme")
    if (nuclei.size() == 1) {
      std::string out;
      for (char32_t c : u) {
        static const std::u32string a = U"áéíóú", p = U"aeiou";
        const size_t k = a.find(c);
        text::appendUtf8(out, k == std::u32string::npos ? c : p[k]);
      }
      return out + std::string(clitics);
    }
    return v + std::string(clitics);
  }
  // stressed nucleus of the bare form: penultimate for words ending in vowel, n, s; else last
  if (nuclei.empty()) return v + std::string(clitics);
  const char32_t last = u.back();
  size_t stressed;
  if (isV(last) || last == U'n' || last == U's') stressed = nuclei.size() >= 2 ? nuclei[nuclei.size() - 2] : nuclei.back();
  else stressed = nuclei.back();
  const size_t total = nuclei.size() + (size_t)added;
  // position of the stressed nucleus from the end of the new word (1 = last)
  size_t pos = 0;
  for (size_t k = 0; k < nuclei.size(); ++k)
    if (nuclei[k] == stressed) pos = total - k;
  // new word ends in a vowel / n / s (clitics do): stress on the penultimate needs no mark
  const bool needs = pos >= 3 || (pos == 1);
  if (!needs || (nuclei.size() == 1 && added <= 1)) return v + std::string(clitics);
  static const std::u32string plain = U"aeiou", acc = U"áéíóú";
  const size_t k = plain.find(u[stressed]);
  if (k != std::u32string::npos) u[stressed] = acc[k];
  return text::toUtf8(u) + std::string(clitics);
}

}  // namespace vp::la2x::detail::es
