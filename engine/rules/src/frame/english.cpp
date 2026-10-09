// English helpers of the frame builder and the transfer (C17). See english.h.
#include "english.h"

#include <algorithm>

#include "vp/features.h"
#include "vp/text.h"

namespace vp::frame::en {
namespace {

using nlp::Token;

bool isIn(const std::string& w, std::initializer_list<const char*> set) {
  for (const char* s : set)
    if (w == s) return true;
  return false;
}

bool endsWith(const std::string& w, const char* suf) {
  const std::string s(suf);
  return w.size() > s.size() && w.compare(w.size() - s.size(), s.size(), s) == 0;
}

uint32_t fget(const Token& t, nlp::morph::Shift s) { return nlp::morph::get(t.feats, s); }

// What the lexicon says about a form.
struct Reading {
  std::string pastOf;          // a verb lemma (other than the form) of which this is a past / past participle
  std::string ingOf;           // ... of which this is the -ing form
  bool finitePast = false;     // the past reading is a finite past ("sang"), not only a participle ("sung")
  bool presentVerb = false;    // a present / infinitive / imperative verb reading exists ("read", "set")
  bool noun = false, nounInflected = false, adj = false, verb = false;
};

Reading readingOf(const lex::Lexicon& lx, const std::string& lower) {
  Reading r;
  std::vector<lex::Analysis> an;
  lx.lookup(text::en_key(lower), an);
  for (const lex::Analysis& a : an) {
    const lex::Lemma l = lx.lemma(a.lemma);
    if (l.id == lex::kNoLemma) continue;
    const feat::Features f = feat::unpack(lx.feature(a.feat));
    const std::string h = text::lower(std::string(l.head));
    if (l.pos == feat::Noun) { r.noun = true; r.nounInflected = r.nounInflected || f.number != 0; }
    if (l.pos == feat::Adj || l.pos == feat::Participle) r.adj = true;
    if (l.pos != feat::Verb) continue;
    r.verb = true;
    if (f.tense == feat::Present || f.mood == feat::Infinitive || f.mood == feat::Imperative) r.presentVerb = true;
    if (h == lower || h.find(' ') != std::string::npos) continue;
    if (f.tense == feat::Perfect || f.tense == feat::Pluperfect) {
      if (r.pastOf.empty()) r.pastOf = h;
      if (h == r.pastOf && (f.mood == 0 || f.mood == feat::Indicative)) r.finitePast = true;
    } else if ((f.mood == feat::Gerund || f.mood == feat::ParticipleMood) && endsWith(lower, "ing")) {
      if (r.ingOf.empty()) r.ingOf = h;
    }
  }
  return r;
}

bool vowel(char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; }

}  // namespace

std::string verbOfForm(const lex::Lexicon& lx, const std::string& lower, bool* present) {
  const Reading r = readingOf(lx, lower);
  if (present) *present = false;
  if (!r.pastOf.empty()) return r.pastOf;
  if (!r.ingOf.empty()) {
    if (present) *present = true;
    return r.ingOf;
  }
  return std::string();
}

std::vector<std::string> baseCandidates(const std::string& w, const std::string& upos) {
  std::vector<std::string> out;
  auto add = [&](const std::string& b) {
    if (b.size() >= 2 && b.size() <= w.size() && b != w && std::find(out.begin(), out.end(), b) == out.end()) out.push_back(b);
  };
  const size_t hy = w.find('-');
  if (hy != std::string::npos && hy > 0 && hy + 1 < w.size()) {
    std::string joined;
    for (char c : w)
      if (c != '-') joined += c;
    add(joined);
    add(w.substr(w.rfind('-') + 1));
    return out;
  }
  const bool any = upos.empty(), noun = any || upos == "NOUN" || upos == "PROPN",
             verb = any || upos == "VERB" || upos == "AUX", adj = any || upos == "ADJ", adv = any || upos == "ADV";
  auto allowed = [&](const std::string& suf) {
    if (suf == "s" || suf == "es" || suf == "ies") return noun || verb;
    if (suf == "ed" || suf == "ied" || suf == "ing") return verb;
    if (suf == "er" || suf == "est" || suf == "ier" || suf == "iest") return adj;
    if (suf == "ly" || suf == "ily") return adv;
    return false;
  };
  auto stem = [&](size_t cut) { return w.substr(0, w.size() - cut); };
  for (const char* suf : {"ies", "ied", "ier", "iest", "ily"})   // y -> i before an ending
    if (endsWith(w, suf) && allowed(suf)) add(stem(std::string(suf).size()) + "y");
  for (const char* suf0 : {"ed", "ing", "er", "est", "es", "s", "ly"}) {
    const std::string suf(suf0);
    if (!endsWith(w, suf0) || !allowed(suf)) continue;
    const std::string b = stem(suf.size());
    if (b.size() < 2) continue;
    if (suf == "s" && (endsWith(w, "ss") || endsWith(w, "us"))) continue;
    // doubled final consonant ("stopped" -> stop, "bigger" -> big)
    if (b.size() >= 3 && b[b.size() - 1] == b[b.size() - 2] && !vowel(b.back()) && b.back() != 's' && b.back() != 'l')
      add(b.substr(0, b.size() - 1));
    add(b);
    if (suf == "ed" || suf == "ing" || suf == "er" || suf == "est") add(b + "e");   // "hoped" -> hope, "braver" -> brave
    if (suf == "ly") add(b + "le");                                                // "gently" -> gentle
  }
  return out;
}

std::string knownBase(const lex::Lexicon& lx, const std::string& lower, const std::string& upos) {
  for (const std::string& b : baseCandidates(lower, upos)) {
    std::vector<lex::Analysis> an;
    lx.lookup(text::en_key(b), an);
    for (const lex::Analysis& a : an) {
      const lex::Lemma l = lx.lemma(a.lemma);
      if (l.id == lex::kNoLemma || l.pos == feat::Name) continue;
      const bool ok = upos.empty() || (upos == "NOUN" && l.pos == feat::Noun) ||
                      ((upos == "VERB" || upos == "AUX") && l.pos == feat::Verb) ||
                      (upos == "ADJ" && (l.pos == feat::Adj || l.pos == feat::Participle || l.pos == feat::Verb)) ||
                      (upos == "ADV" && (l.pos == feat::Adv || l.pos == feat::Adj)) ||
                      (upos == "PROPN" && (l.pos == feat::Noun || l.pos == feat::Adj));
      if (ok) return text::lower(std::string(l.head)) == b ? b : text::lower(std::string(l.head));
    }
  }
  return std::string();
}

bool compoundParts(const lex::Lexicon& lx, const std::string& w, std::string& first, std::string& head) {
  static const std::pair<const char*, const char*> kHeads[] = {
      {"women", "woman"}, {"woman", "woman"}, {"men", "man"},     {"man", "man"},       {"maids", "maid"},
      {"maid", "maid"},   {"boys", "boy"},    {"boy", "boy"},     {"girls", "girl"},    {"girl", "girl"},
      {"smiths", "smith"}, {"smith", "smith"}, {"keepers", "keeper"}, {"keeper", "keeper"}, {"makers", "maker"},
      {"maker", "maker"}, {"castles", "castle"}, {"castle", "castle"}, {"houses", "house"}, {"house", "house"},
      {"folk", "folk"},   {"birds", "bird"},  {"bird", "bird"},   {"cakes", "cake"},    {"cake", "cake"},
      {"balls", "ball"},  {"ball", "ball"},   {"flies", "fly"},   {"fly", "fly"},       {"worms", "worm"},
      {"worm", "worm"},   {"rooms", "room"},  {"room", "room"},   {"yards", "yard"},    {"yard", "yard"},
      {"trees", "tree"},  {"tree", "tree"},   {"berries", "berry"}, {"berry", "berry"}, {"stones", "stone"},
      {"stone", "stone"}, {"pots", "pot"},    {"pot", "pot"},
      // C26: the Oz register ("housetop", "hilltop", "hillside", "wayside", "riverbank", "cornfield", "doorway")
      {"tops", "top"},    {"top", "top"},     {"sides", "side"},  {"side", "side"},     {"banks", "bank"},
      {"bank", "bank"},   {"fields", "field"}, {"field", "field"}, {"ways", "way"},     {"way", "way"},
      {"paths", "path"},  {"path", "path"},   {"lands", "land"},  {"land", "land"}};
  auto known = [&](const std::string& x, bool needNoun) {
    std::vector<lex::Analysis> an;
    lx.lookup(text::en_key(x), an);
    for (const lex::Analysis& a : an) {
      const lex::Lemma l = lx.lemma(a.lemma);
      if (l.id == lex::kNoLemma || l.pos == feat::Name) continue;
      if (!needNoun || l.pos == feat::Noun) return true;
    }
    return false;
  };
  for (const auto& h : kHeads) {
    if (!endsWith(w, h.first)) continue;
    const std::string a = w.substr(0, w.size() - std::string(h.first).size());
    if (a.size() < 2) continue;
    if (!known(a, false) || !known(h.second, true)) continue;
    first = a;
    head = h.second;
    return true;
  }
  return false;
}

// C24: kinship words and titles are nouns of address even when the lexicon also lists a verb ("to father")
bool addressWord(const std::string& w) {
  return isIn(w, {"grandfather", "grandmother", "grandpa", "grandma", "father", "mother", "uncle", "aunt", "brother",
                  "sister", "son", "daughter", "master", "mistress", "doctor", "captain", "nurse", "cousin", "teacher",
                  "boy", "girl", "friend", "children", "boys", "girls", "friends"});
}

bool retagForms(std::vector<Token>& tk, const lex::Lexicon& lx) {
  bool changed = false;
  const int n = (int)tk.size();
  // C24: a capitalised first word before a comma tagged as a verb that the lexicon knows only as a noun
  // ("Grandfather, are you tired?"): the noun of address
  if (n >= 3 && tk[0].upos == "VERB" && tk[1].text == "," && !tk[0].text.empty() && tk[0].text[0] >= 'A' &&
      tk[0].text[0] <= 'Z') {
    std::vector<lex::Analysis> an;
    lx.lookup(text::en_key(tk[0].lower), an);
    bool noun = false, verb = false;
    for (const lex::Analysis& a : an) {
      const uint8_t ps = lx.lemma(a.lemma).pos;
      noun = noun || ps == feat::Noun;
      verb = verb || ps == feat::Verb;
    }
    if (noun && (!verb || addressWord(tk[0].lower))) {
      tk[0].upos = "NOUN";
      tk[0].feats = nlp::morph::fromString("Number=Sing");
      changed = true;
    }
  }
  // C19: a cue that starts with a preposition or a coordinator in lower case ("with a loud cry.", "to the little
  // house.", "and the queen's crown,"): the tagger reads the word as an imperative verb. A word of these closed lists
  // at the start (or right after a coordinator) before the start of a noun phrase is a preposition / coordinator.
  for (int i = 0; i + 1 < n; ++i) {
    Token& t = tk[(size_t)i];
    if (t.upos != "VERB" && t.upos != "NOUN" && t.upos != "PROPN" && t.upos != "ADJ") continue;
    const bool start = i == 0 || (i == 1 && (tk[0].upos == "CCONJ" || tk[0].lower == "and" || tk[0].lower == "or" ||
                                             tk[0].lower == "but" || tk[0].lower == "nor"));
    if (!start) continue;
    const std::string& nx = tk[(size_t)i + 1].upos;
    const bool npNext = isIn(nx, {"DET", "PRON", "NOUN", "PROPN", "ADJ", "NUM"}) || tk[(size_t)i + 1].lower == "his" ||
                        tk[(size_t)i + 1].lower == "her" || tk[(size_t)i + 1].lower == "the";
    if (isIn(t.lower, {"and", "or", "nor", "but"})) {
      if (t.upos == "VERB" || t.upos == "NOUN" || t.upos == "PROPN") {
        t.upos = "CCONJ";
        t.feats = 0;
        changed = true;
      }
      continue;
    }
    if (!npNext) continue;
    if (isIn(t.lower, {"with", "in", "into", "to", "from", "for", "under", "without", "at", "by", "onto", "upon",
                       "behind", "among", "amongst", "between", "beneath", "beside", "inside", "against", "toward",
                       "towards", "across", "through", "during", "of", "near", "after", "before", "over", "around",
                       "above", "below", "beyond", "along", "outside"})) {
      t.upos = "ADP";
      t.feats = 0;
      changed = true;
    }
  }
  // C22: after a form of "be" (or "feel", "seem", "look", "get", "become"), a word in -ly that english.vpl knows only as an
  // adjective ("I am lonely", "She looks lovely") is the adjective, and a word it knows only as a noun ("That is
  // nonsense") is a noun; the tagger read them as an adverb / an adjective
  for (int i = 1; i < n; ++i) {
    Token& t = tk[(size_t)i];
    int p = i - 1;
    while (p > 0 && isIn(tk[(size_t)p].lower, {"so", "very", "too", "really", "quite", "awfully", "rather", "not", "n't"})) --p;
    const std::string& pl = tk[(size_t)p].lower;
    const bool copula = isIn(pl, {"am", "is", "are", "was", "were", "be", "been", "being", "'m", "'s", "'re", "feel",
                                  "feels", "felt", "seem", "seems", "seemed", "look", "looks", "looked", "become",
                                  "became", "get", "got", "gets"});
    if (!copula) continue;
    if (t.upos == "ADV" && endsWith(t.lower, "ly")) {
      const Reading r = readingOf(lx, t.lower);
      // closing the clause right after the copula ("That's silly.", "It is lovely."): the adjective
      const bool closing = i + 1 >= n || isIn(tk[(size_t)i + 1].upos, {"PUNCT", "CCONJ"});
      if (r.adj && closing && p == i - 1 && isIn(pl, {"am", "is", "are", "was", "were", "be", "been", "'m", "'s", "'re"})) {
        t.upos = "ADJ";
        t.feats = 0;
        changed = true;
      } else if (r.adj && !r.noun && !r.verb) {
        bool advReading = false;
        std::vector<lex::Analysis> an;
        lx.lookup(text::en_key(t.lower), an);
        for (const lex::Analysis& a : an) advReading = advReading || lx.lemma(a.lemma).pos == feat::Adv;
        if (!advReading) { t.upos = "ADJ"; t.feats = 0; changed = true; }
      }
    } else if (t.upos == "ADJ" && (i + 1 >= n || isIn(tk[(size_t)i + 1].upos, {"PUNCT", "CCONJ"}))) {
      const Reading r = readingOf(lx, t.lower);
      if (r.noun && !r.adj && !r.verb) { t.upos = "NOUN"; t.feats = nlp::morph::fromString("Number=Sing"); changed = true; }
    }
  }
  // C22: a word tagged as a verb that english.vpl knows only as an adjective ("The bear has thick fur": "has thick"
  // read as a perfect) is the adjective, and the "have" before it the main verb
  for (int i = 1; i + 1 < n; ++i) {
    Token& t = tk[(size_t)i];
    if (t.upos != "VERB") continue;
    const Reading r = readingOf(lx, t.lower);
    if (!r.adj) continue;
    if (!isIn(tk[(size_t)i + 1].upos, {"NOUN", "PROPN"})) continue;
    if (r.verb) {   // a verb reading too: only when it cannot be a participle after have ("has thick fur")
      if (!isIn(tk[(size_t)i - 1].lower, {"has", "have", "had"})) continue;
      bool part = false;
      std::vector<lex::Analysis> an;
      lx.lookup(text::en_key(t.lower), an);
      for (const lex::Analysis& a : an) {
        const feat::Features f = feat::unpack(lx.feature(a.feat));
        part = part || (lx.lemma(a.lemma).pos == feat::Verb && (f.tense == feat::Perfect || f.mood == feat::ParticipleMood));
      }
      if (part || endsWith(t.lower, "ed")) continue;
    }
    t.upos = "ADJ";
    t.feats = 0;
    Token& p = tk[(size_t)i - 1];
    if (p.upos == "AUX" && isIn(p.lower, {"has", "have", "had"})) {
      p.upos = "VERB";
      p.feats = nlp::morph::fromString(p.lower == "had" ? "Tense=Past|VerbForm=Fin|Mood=Ind"
                                                       : p.lower == "has" ? "Number=Sing|Person=3|Tense=Pres|VerbForm=Fin|Mood=Ind"
                                                                          : "Tense=Pres|VerbForm=Fin|Mood=Ind");
    }
    changed = true;
  }
  // C22: a word right after my / your / our / their / its tagged as a verb that english.vpl knows as a plural or
  // singular noun is that noun ("My ears and whiskers!")
  for (int i = 1; i < n; ++i) {
    Token& t = tk[(size_t)i];
    if (t.upos != "VERB" || !isIn(tk[(size_t)i - 1].lower, {"my", "your", "our", "their", "its"})) continue;
    const Reading r = readingOf(lx, t.lower);
    if (!r.noun) continue;
    bool plural = false;
    std::vector<lex::Analysis> an;
    lx.lookup(text::en_key(t.lower), an);
    for (const lex::Analysis& a : an)
      if (lx.lemma(a.lemma).pos == feat::Noun && feat::unpack(lx.feature(a.feat)).number == feat::Pl) plural = true;
    t.upos = "NOUN";
    t.feats = nlp::morph::fromString(plural ? "Number=Plur" : "Number=Sing");
    changed = true;
  }
  // C22: "be (just) like X": "like" after a form of "be" is the preposition, never the verb ("You'd be just like
  // people" had "like" as the root verb)
  for (int i = 1; i + 1 < n; ++i) {
    Token& t = tk[(size_t)i];
    if (t.lower != "like" || t.upos != "VERB") continue;
    int p = i - 1;
    while (p > 0 && isIn(tk[(size_t)p].lower, {"just", "exactly", "so", "very", "much", "rather", "quite", "a", "bit", "little", "not", "n't"})) --p;
    if (!isIn(tk[(size_t)p].lower, {"be", "is", "am", "are", "was", "were", "been", "being", "'s", "'re", "'m"})) continue;
    t.upos = "ADP";
    t.feats = 0;
    changed = true;
  }
  // C22: a bare word after "and / or" coordinated with a verb that has a modal ("Dogs would sing and dance all day"):
  // the second verb, not a noun ("dance" was the subject of a fragment)
  for (int i = 2; i < n; ++i) {
    Token& t = tk[(size_t)i];
    if (t.upos != "NOUN" || t.deprel != "conj" || t.head <= 0 || !isIn(tk[(size_t)i - 1].lower, {"and", "or"})) continue;
    const int v = t.head - 1;
    if (v < 0 || v >= i || tk[(size_t)v].upos != "VERB") continue;
    bool modal = false, det = false;
    for (int j = 0; j < n; ++j) {
      if (tk[(size_t)j].head == v + 1 && tk[(size_t)j].deprel == "aux" &&
          isIn(tk[(size_t)j].lower, {"would", "will", "can", "could", "should", "must", "may", "might", "shall", "'d", "'ll"}))
        modal = true;
      if (tk[(size_t)j].head == v + 1 && j < v && tk[(size_t)j].lower == "to" && isIn(tk[(size_t)j].deprel, {"mark", "aux"}))
        modal = true;   // "wants to sing and dance
      if (tk[(size_t)j].head == i + 1 && (tk[(size_t)j].deprel == "det" || tk[(size_t)j].deprel == "amod")) det = true;
    }
    if (!modal || det) continue;
    bool base = false;   // a verb whose headword is this very form ("dance", "sing")
    {
      std::vector<lex::Analysis> an;
      lx.lookup(text::en_key(t.lower), an);
      for (const lex::Analysis& a : an) {
        const lex::Lemma l = lx.lemma(a.lemma);
        base = base || (l.pos == feat::Verb && text::lower(std::string(l.head)) == t.lower);
      }
    }
    if (!base) continue;
    t.upos = "VERB";
    t.feats = nlp::morph::fromString("VerbForm=Inf");
    changed = true;
  }
  bool anyVerb = false;
  for (const Token& t : tk) anyVerb = anyVerb || t.upos == "VERB";
  auto nominal = [&](int i) { return i >= 0 && isIn(tk[(size_t)i].upos, {"NOUN", "PROPN", "PRON"}); };
  // a plural noun read as a verb before its own verb ("Only witches wear black hats.", "and only witches and
  // sorceresses wear white."): not after a subject, and a verb follows (after "and" + nouns)
  for (int i = 0; i + 1 < n; ++i) {
    Token& t = tk[(size_t)i];
    if (t.upos != "VERB" || t.lower.size() < 4 || t.lower.back() != 's') continue;
    int p = i - 1;
    while (p >= 0 && (tk[(size_t)p].lower == "only" || tk[(size_t)p].lower == "even" || tk[(size_t)p].lower == "all")) --p;
    if (p >= 0 && !isIn(tk[(size_t)p].upos, {"CCONJ", "PUNCT", "DET", "ADJ", "SCONJ", "ADP"})) continue;
    std::vector<lex::Analysis> an;
    lx.lookup(text::en_key(t.lower), an);
    bool plural = false;
    for (const lex::Analysis& a : an)
      if (lx.lemma(a.lemma).pos == feat::Noun && feat::unpack(lx.feature(a.feat)).number == feat::Pl) plural = true;
    if (!plural) continue;
    int j = i + 1;
    while (j + 1 < n && tk[(size_t)j].upos == "CCONJ" && isIn(tk[(size_t)j + 1].upos, {"NOUN", "PROPN"})) j += 2;
    if (j >= n || tk[(size_t)j].upos == "PUNCT") continue;
    std::vector<lex::Analysis> vn;
    lx.lookup(text::en_key(tk[(size_t)j].lower), vn);
    bool verbNext = false;
    for (const lex::Analysis& a : vn) {
      const feat::Features f = feat::unpack(lx.feature(a.feat));
      if (lx.lemma(a.lemma).pos == feat::Verb &&
          (f.tense == feat::Present || f.tense == feat::Perfect || text::lower(std::string(lx.lemma(a.lemma).head)) == tk[(size_t)j].lower))
        verbNext = true;
    }
    if (!verbNext) continue;
    t.upos = "NOUN";
    t.feats = nlp::morph::fromString("Number=Plur");
    Token& v = tk[(size_t)j];
    if (v.upos != "VERB") {
      v.upos = "VERB";
      v.feats = nlp::morph::fromString("Number=Plur|Person=3|Tense=Pres|VerbForm=Fin|Mood=Ind");
    }
    changed = true;
  }
  // C19: a sentence-initial word before a comma that the lexicon knows as a verb (base form) and not as a name is an
  // imperative ("Hurry, the ship is leaving!", "Run, the bear is coming!"), never a name candidate; a common noun there
  // is the person addressed ("Grandmother, may we ...?")
  // C20: also a word the tagger took for an interjection ("Hide, the witch is here!" lost "Hide" and was OK) when the
  // lexicon has no interjection of that spelling
  const bool intj0 = n >= 3 && tk[0].upos == "INTJ" && tk[1].text == ",";
  if (n >= 3 && (tk[0].upos == "PROPN" || intj0) && tk[1].text == "," && !tk[0].text.empty()) {
    std::vector<lex::Analysis> an;
    lx.lookup(text::en_key(tk[0].lower), an);
    bool verb = false, name = false, noun = false, intj = false;
    for (const lex::Analysis& a : an) {
      const lex::Lemma l = lx.lemma(a.lemma);
      if (l.pos == feat::Name) name = true;
      if (l.pos == feat::Verb && text::lower(std::string(l.head)) == tk[0].lower) verb = true;
      if (l.pos == feat::Noun) noun = true;
      if (l.pos == feat::Intj) intj = true;
    }
    if (intj0 && (intj || !verb)) {
      // a real interjection ("Oh,", "Hey,"): unchanged
    } else if (verb && !name && !(noun && addressWord(tk[0].lower))) {   // C24: "Grandfather, ..." is no order
      tk[0].upos = "VERB";
      tk[0].feats = nlp::morph::fromString("VerbForm=Fin|Mood=Imp");
      changed = true;
    } else if (noun && !name) {
      tk[0].upos = "NOUN";
      tk[0].feats = nlp::morph::fromString("Number=Sing");
      changed = true;
    }
  }
  // C20: a past form between a possessive and a noun ("She found her lost ring.", "He mended his broken toy.") is a
  // participle used as an adjective, not the verb of a clause whose subject is "her"
  for (int i = 1; i + 1 < n; ++i) {
    Token& t = tk[(size_t)i];
    if (t.upos != "VERB" || tk[(size_t)i + 1].upos != "NOUN") continue;
    if (!isIn(tk[(size_t)i - 1].lower, {"my", "your", "his", "her", "its", "our", "their"})) continue;
    const Reading r = readingOf(lx, t.lower);
    if (r.pastOf.empty() || r.presentVerb) continue;
    t.upos = "ADJ";
    t.feats = nlp::morph::fromString("VerbForm=Part|Tense=Past");
    changed = true;
  }
  // C19: "Nobody knows where the dragon lives.": an -s word after "wh / subordinator + the + noun" that the lexicon
  // knows as a verb is that clause's verb (lives, not the plural of life)
  for (int i = 3; i < n; ++i) {
    Token& t = tk[(size_t)i];
    if (t.upos != "NOUN" || t.lower.size() < 4 || t.lower.back() != 's') continue;
    if (tk[(size_t)i - 1].upos != "NOUN" || tk[(size_t)i - 2].upos != "DET") continue;
    if (!isIn(tk[(size_t)i - 3].lower, {"where", "when", "why", "how", "what", "if", "because", "that", "until", "while",
                                        "whether", "before", "after"}))
      continue;
    if (i + 1 < n && !isIn(tk[(size_t)i + 1].upos, {"PUNCT", "ADV", "ADP", "DET", "PRON"})) continue;
    std::vector<lex::Analysis> an;
    lx.lookup(text::en_key(t.lower), an);
    bool verb3 = false;
    for (const lex::Analysis& a : an) {
      const feat::Features f = feat::unpack(lx.feature(a.feat));
      if (lx.lemma(a.lemma).pos == feat::Verb && f.person == 3 && f.number == feat::Sg) verb3 = true;
    }
    if (!verb3) continue;
    t.upos = "VERB";
    t.feats = nlp::morph::fromString("Number=Sing|Person=3|Tense=Pres|VerbForm=Fin|Mood=Ind");
    changed = true;
  }
  // a capitalised word the lexicon does not know, before a verb or inside the sentence, is a name ("Grimbly ate the
  // cake."): kept as written (names_la.tsv policy: indeclinable, Check)
  for (int i = 0; i < n; ++i) {
    Token& t = tk[(size_t)i];
    if (t.upos == "PROPN" || t.text.empty() || !(t.text[0] >= 'A' && t.text[0] <= 'Z') || t.text.size() < 2) continue;
    if (t.text.size() > 1 && t.text[1] >= 'A' && t.text[1] <= 'Z') continue;   // abbreviations
    std::vector<lex::Analysis> an;
    lx.lookup(text::en_key(t.lower), an);
    if (!an.empty() || !knownBase(lx, t.lower, std::string()).empty()) continue;
    const bool verbNext = i + 1 < n && isIn(tk[(size_t)i + 1].upos, {"VERB", "AUX"});
    if (i == 0 && !verbNext) continue;
    t.upos = "PROPN";
    t.feats = nlp::morph::fromString("Number=Sing");
    changed = true;
  }
  for (int i = 0; i < n; ++i) {
    Token& t = tk[(size_t)i];
    if (t.lower.empty() || t.text.empty()) continue;
    // C19: a word right after a possessive determiner is a noun, not a verb ("without his hat or his coat": not
    // "hit"), when the lexicon has a noun reading
    if (t.upos == "VERB" && i > 0 &&
        isIn(tk[(size_t)i - 1].lower, {"my", "your", "his", "its", "our", "their"}) &&
        (i + 1 >= n || !isIn(tk[(size_t)i + 1].upos, {"DET", "PRON"}))) {
      const Reading r = readingOf(lx, t.lower);
      if (r.noun) {
        t.upos = "NOUN";
        t.feats = nlp::morph::fromString(t.lower.size() > 3 && t.lower.back() == 's' && r.nounInflected ? "Number=Plur" : "Number=Sing");
        changed = true;
        continue;
      }
    }
    // C19: a predicate after "be" without a determiner that the lexicon knows as an adjective ("a person who is kind",
    // "The night was cold"): an adjective, not a noun (kind -> genus)
    if (t.upos == "NOUN" && i > 0 && isIn(tk[(size_t)i - 1].lower, {"is", "are", "was", "were", "am", "be", "been", "very",
                                                                     "so", "too", "quite"}) &&
        (i + 1 >= n || isIn(tk[(size_t)i + 1].upos, {"PUNCT", "CCONJ"}) || tk[(size_t)i + 1].lower == "to")) {
      const Reading r = readingOf(lx, t.lower);
      if (r.adj && !r.nounInflected) {
        t.upos = "ADJ";
        t.feats = 0;
        changed = true;
        continue;
      }
    }
    // a noun tagged as a verb right after a determiner and before the verb ("when the clown fell down")
    if (t.upos == "VERB" && i > 0 && i + 1 < n && tk[(size_t)i - 1].upos == "DET" &&
        isIn(tk[(size_t)i + 1].upos, {"VERB", "AUX"}) && !isIn(tk[(size_t)i - 1].lower, {"that", "this"})) {
      const Reading r = readingOf(lx, t.lower);
      if (r.noun) {
        t.upos = "NOUN";
        t.feats = nlp::morph::fromString("Number=Sing");
        changed = true;
        continue;
      }
    }
    if (t.upos == "VERB") {
      // an irregular past read as a present ("She sang loudly.")
      const uint32_t tt = fget(t, nlp::morph::TenseShift), vf = fget(t, nlp::morph::VerbFormShift);
      // C30: a gerund tag on a form that is none ("The birds sang in the trees." had sang as a gerund
      // and the clause in the present) is the tagger's slip: the past is checked below
      const bool slip = vf == nlp::morph::VfGer && !endsWith(t.lower, "ing");
      if (tt == nlp::morph::TensePast || vf == nlp::morph::VfPart ||
          ((vf == nlp::morph::VfGer || vf == nlp::morph::VfInf) && !slip))
        continue;
      if (fget(t, nlp::morph::MoodShift) == nlp::morph::MoodImp) continue;
      const Reading r = readingOf(lx, t.lower);
      if (r.pastOf.empty() || r.presentVerb) continue;
      bool aux = false;
      for (int j = std::max(0, i - 3); j < i; ++j)
        aux = aux || isIn(tk[(size_t)j].lower, {"have", "has", "had", "'ve", "'d", "be", "been", "being", "was", "were",
                                                "is", "are", "am", "'s", "'re", "'m"});
      std::string ud;
      const uint32_t num = fget(t, nlp::morph::NumberShift), per = fget(t, nlp::morph::PersonShift);
      if (aux || !r.finitePast) {
        ud = "Tense=Past|VerbForm=Part";
      } else {
        if (num) ud += num == nlp::morph::NumPlur ? "Number=Plur|" : "Number=Sing|";
        if (per) ud += "Person=" + std::to_string((int)per) + "|";
        ud += "Tense=Past|VerbForm=Fin|Mood=Ind";
      }
      t.feats = nlp::morph::fromString(ud);
      changed = true;
      continue;
    }
    // an adjective tagged as an adverb: between a determiner and a noun ("a clever girl"), or after "too" / "so" /
    // "very" ("The fox was too clever"), when the lexicon has no adverb reading
    const bool degreeBefore = i > 0 && isIn(tk[(size_t)i - 1].lower, {"too", "so", "very", "quite", "rather"});
    if (t.upos == "ADV" && i > 0 && ((i + 1 < n && tk[(size_t)i - 1].upos == "DET" && tk[(size_t)i + 1].upos == "NOUN") ||
                                     degreeBefore)) {
      std::vector<lex::Analysis> an;
      lx.lookup(text::en_key(t.lower), an);
      bool adj = false, adv = false;
      for (const lex::Analysis& a : an) {
        const uint8_t p = lx.lemma(a.lemma).pos;
        adj = adj || p == feat::Adj;
        adv = adv || p == feat::Adv;
      }
      if (adj && !adv) { t.upos = "ADJ"; t.feats = 0; changed = true; }
      continue;
    }
    if ((t.upos == "NOUN" || t.upos == "X") && i > 0) {
      const Reading r = readingOf(lx, t.lower);
      // a past tagged as a noun in a sentence without a verb ("The bell rang.", "The witch and the wizard sang.")
      // C19: not after a possessive ("Where is my hat?": hat is no past of "hit") nor beside an auxiliary verb
      bool aux = false;
      for (const Token& x : tk) aux = aux || x.upos == "AUX";
      // C19: a clause opened by a subordinator and closed by a comma ("When the sun rose, we went ...", "Before the
      // sun set, ...") is a clause of its own: "rose" / "set" after its subject is its verb even when the main clause
      // has one (the past of rise, not the flower; set as a past)
      int segA = i, segB = i;
      while (segA > 0 && tk[(size_t)segA - 1].text != ",") --segA;
      while (segB + 1 < n && tk[(size_t)segB + 1].text != ",") ++segB;
      bool segVerb = false;
      for (int q = segA; q <= segB; ++q) segVerb = segVerb || tk[(size_t)q].upos == "VERB" || tk[(size_t)q].upos == "AUX";
      const bool subClause = segA == 0 && segB + 1 < n && !segVerb && i == segB &&
                             isIn(tk[0].lower, {"when", "after", "before", "while", "as", "until", "till", "once", "since",
                                                "if", "because"}) &&
                             (nominal(i - 1) || (i >= 2 && tk[(size_t)i - 1].upos == "ADV" && nominal(i - 2)));
      // C20: the same clause at the end of the sentence ("We sang songs until the moon rose.", "We waited till the sun
      // set."): a subordinator inside the sentence, a noun, and a past form closing the sentence
      bool tailClause = false;
      // C30: also before a closing place phrase ("until the moon rose above the trees.")
      bool ppTail = i + 1 < n && tk[(size_t)i + 1].upos == "ADP";
      for (int q = i + 1; ppTail && q < n; ++q)
        ppTail = isIn(tk[(size_t)q].upos, {"ADP", "DET", "ADJ", "NOUN", "PROPN", "NUM"}) || (q + 1 == n && tk[(size_t)q].upos == "PUNCT");
      if (!subClause && !r.pastOf.empty() && r.finitePast && !r.nounInflected && i + 1 < n && i >= 3 &&
          (tk[(size_t)i + 1].upos == "PUNCT" || ppTail) && (i + 2 == n || tk[(size_t)i + 1].text != ",") && nominal(i - 1)) {
        int q = i - 1;
        while (q > 0 && (nominal(q) || isIn(tk[(size_t)q].upos, {"DET", "ADJ", "NUM"}))) --q;
        tailClause = q > 0 && q < i - 1 && isIn(tk[(size_t)q].lower, {"until", "till", "when", "after", "before", "while", "since"});
      }
      if ((subClause || tailClause) && (!r.pastOf.empty() || r.presentVerb) && !r.nounInflected) {
        bool pastMain = false;
        for (int q = segB + 1; q < n; ++q)
          pastMain = pastMain || fget(tk[(size_t)q], nlp::morph::TenseShift) == nlp::morph::TensePast;
        t.upos = "VERB";
        t.feats = nlp::morph::fromString(!r.pastOf.empty() || pastMain ? "Tense=Past|VerbForm=Fin|Mood=Ind"
                                                                        : "Number=Sing|Person=3|Tense=Pres|VerbForm=Fin|Mood=Ind");
        changed = true;
        continue;
      }
      if (!anyVerb && !aux && !r.pastOf.empty() && r.finitePast && !r.presentVerb && !r.nounInflected &&
          !isIn(tk[(size_t)i - 1].lower, {"my", "your", "his", "her", "its", "our", "their"}) &&
          (nominal(i - 1) || (i >= 2 && tk[(size_t)i - 1].upos == "ADV" && nominal(i - 2)))) {
        t.upos = "VERB";
        t.feats = nlp::morph::fromString("Tense=Past|VerbForm=Fin|Mood=Ind");
        anyVerb = true;
        changed = true;
        continue;
      }
      // a word the lexicon knows only as an adjective ("braver", "kinder"), or as an adjective and a rare bare noun
      // between a determiner and a noun ("a clever girl")
      const bool between = i + 1 < n && tk[(size_t)i - 1].upos == "DET" && tk[(size_t)i + 1].upos == "NOUN";
      if ((!r.noun || (between && !r.nounInflected)) && r.adj && !r.verb) {
        t.upos = "ADJ";
        t.feats = 0;
        changed = true;
      }
    }
  }
  // C26: a word tagged as an adjective that the parser gave an object ("The song of the bird pleased everyone.") and
  // that english.vpl knows as a finite past is the verb
  for (int i = 1; i < n; ++i) {
    Token& t = tk[(size_t)i];
    if (t.upos != "ADJ") continue;
    // not after a determiner or a possessive ("her lost ring": the participle before its noun)
    if (tk[(size_t)i - 1].upos == "DET" || isIn(tk[(size_t)i - 1].lower, {"my", "your", "his", "her", "its", "our", "their"}))
      continue;
    bool obj = false;
    for (int j = i + 1; j < n; ++j) obj = obj || (tk[(size_t)j].head == i + 1 && tk[(size_t)j].deprel == "obj");
    if (!obj) continue;
    const Reading r = readingOf(lx, t.lower);
    if (r.pastOf.empty() || !r.finitePast) continue;
    t.upos = "VERB";
    t.feats = nlp::morph::fromString("Tense=Past|VerbForm=Fin|Mood=Ind");
    changed = true;
  }
  // C26: a lower-case word tagged as a proper name that english.vpl knows as a common noun is that noun ("The road
  // of yellow brick." had "brick" as a name); a colour word tagged as a noun before "and" / "," and another
  // adjective or colour of the same noun is an adjective ("the pink and orange sky")
  for (int i = 0; i < n; ++i) {
    Token& t = tk[(size_t)i];
    if (t.upos == "PROPN" && !t.text.empty() && t.text[0] >= 'a' && t.text[0] <= 'z') {
      const Reading r = readingOf(lx, t.lower);
      if (r.noun) {
        t.upos = "NOUN";
        t.feats = nlp::morph::fromString(endsWith(t.lower, "s") && r.nounInflected ? "Number=Plur" : "Number=Sing");
        changed = true;
      }
    }
    if (t.upos == "NOUN" && colourWord(t.lower) && i + 2 < n && (tk[(size_t)i + 1].lower == "and" || tk[(size_t)i + 1].text == ",") &&
        (tk[(size_t)i + 2].upos == "ADJ" || colourWord(tk[(size_t)i + 2].lower))) {
      int k = i + 2;
      while (k < n && (tk[(size_t)k].upos == "ADJ" || colourWord(tk[(size_t)k].lower) || tk[(size_t)k].lower == "and")) ++k;
      if (k < n && isIn(tk[(size_t)k].upos, {"NOUN", "PROPN"}) && k > i + 2) {
        for (int j = i; j < k; ++j)
          if (colourWord(tk[(size_t)j].lower) && tk[(size_t)j].upos != "ADJ") { tk[(size_t)j].upos = "ADJ"; tk[(size_t)j].feats = 0; }
        changed = true;
      }
    }
  }
  // C26: "A beautiful silk dress.": a verbless noun phrase whose last word the tagger read as a verb after a noun or
  // an adjective is a noun when english.vpl lists one
  {
    int verbs = 0, v = -1;
    for (int i = 0; i < n; ++i)
      if (tk[(size_t)i].upos == "VERB" || tk[(size_t)i].upos == "AUX") { ++verbs; v = i; }
    int last = n - 1;
    while (last > 0 && tk[(size_t)last].upos == "PUNCT") --last;
    if (verbs == 1 && v == last && v >= 2 && isIn(tk[0].lower, {"a", "an"}) &&
        isIn(tk[(size_t)v - 1].upos, {"NOUN", "ADJ"}) && tk[(size_t)n - 1].text != "?" &&
        fget(tk[(size_t)v - 1], nlp::morph::NumberShift) != nlp::morph::NumPlur) {
      bool np = true;
      for (int i = 1; i < v; ++i) np = np && isIn(tk[(size_t)i].upos, {"DET", "ADJ", "NOUN", "NUM", "ADV"});
      const Reading r = readingOf(lx, tk[(size_t)v].lower);
      if (np && r.noun && !r.finitePast && !endsWith(tk[(size_t)v].lower, "ed") && !endsWith(tk[(size_t)v].lower, "s")) {
        tk[(size_t)v].upos = "NOUN";
        tk[(size_t)v].feats = nlp::morph::fromString(r.nounInflected && endsWith(tk[(size_t)v].lower, "s") ? "Number=Plur" : "Number=Sing");
        changed = true;
      }
    }
  }
  // C26: "Just an old box.", "Only a little bird.": a sentence-initial restricting word before a noun phrase is the
  // adverb (the tagger read "just" as a verb)
  if (n >= 3 && isIn(tk[0].lower, {"just", "only", "merely", "simply"}) && tk[0].upos != "ADV" &&
      isIn(tk[1].upos, {"DET", "ADJ", "NUM", "NOUN", "PROPN", "PRON"})) {
    bool verb = false;
    for (int i = 1; i < n; ++i) verb = verb || tk[(size_t)i].upos == "VERB" || tk[(size_t)i].upos == "AUX";
    if (!verb) {
      tk[0].upos = "ADV";
      tk[0].feats = 0;
      changed = true;
    }
  }
  // C26: a word tagged as an adverb that modifies a noun ("Two pretty girls": amod) and that english.vpl knows as an
  // adjective is the adjective
  for (int i = 0; i + 1 < n; ++i) {
    Token& t = tk[(size_t)i];
    if (t.upos != "ADV" || t.deprel != "amod" || t.head <= 0 || t.head - 1 >= n) continue;
    if (!isIn(tk[(size_t)t.head - 1].upos, {"NOUN", "PROPN"})) continue;
    const Reading r = readingOf(lx, t.lower);
    if (!r.adj) continue;
    t.upos = "ADJ";
    t.feats = 0;
    changed = true;
  }
  // C26: a verb whose past is its bare form ("let", "put", "cut", "hit", "set", "shut", "hurt") with a 3rd-person
  // singular subject and no auxiliary is the past ("She never let the dog in" was the present sinit)
  for (int i = 1; i < n; ++i) {
    Token& t = tk[(size_t)i];
    if (t.upos != "VERB" || !isIn(t.lower, {"let", "put", "cut", "hit", "set", "shut", "hurt", "cost", "spread", "burst",
                                            "cast", "split", "shed", "quit"}))
      continue;
    if (fget(t, nlp::morph::TenseShift) == nlp::morph::TensePast) continue;
    int subj = -1;
    bool aux = false;
    for (int j = 0; j < n; ++j) {
      if (tk[(size_t)j].head != i + 1) continue;
      if (tk[(size_t)j].deprel == "nsubj" && j < i) subj = j;
      if (tk[(size_t)j].deprel == "aux" || tk[(size_t)j].deprel == "mark") aux = true;
    }
    if (subj < 0 || aux) continue;
    const Token& sj = tk[(size_t)subj];
    const bool third = isIn(sj.lower, {"he", "she", "it"}) || sj.upos == "PROPN" ||
                       (sj.upos == "NOUN" && fget(sj, nlp::morph::NumberShift) != nlp::morph::NumPlur);
    if (!third) continue;
    t.feats = nlp::morph::fromString("Tense=Past|VerbForm=Fin|Mood=Ind");
    changed = true;
  }
  // C26: "tomorrow" / "yesterday" / "today" / "tonight" in a sentence with a verb of its own are the time adverbs
  // (crās, herī, hodiē), never a noun subject ("Tomorrow the king will arrive." was "prōcrāstinātiō rēx perveniō", a
  // Fix): not after a determiner, a possessive or a preposition, and not as the subject of "be" ("Tomorrow is ...")
  {
    bool verb = false;
    for (const Token& t : tk) verb = verb || t.upos == "VERB" || (t.upos == "AUX" && !isIn(t.lower, {"is", "was", "'s"}));
    for (int i = 0; verb && i < n; ++i) {
      Token& t = tk[(size_t)i];
      if (!isIn(t.lower, {"tomorrow", "yesterday", "today", "tonight"}) || t.upos == "ADV") continue;
      if (i > 0 && tk[(size_t)i - 1].text != "," && tk[(size_t)i - 1].text != ";") continue;   // sentence or clause start
      if (i > 0 && isIn(tk[(size_t)i - 1].upos, {"DET", "ADP"})) continue;
      if (i > 0 && isIn(tk[(size_t)i - 1].lower, {"my", "your", "his", "her", "its", "our", "their", "of"})) continue;
      if (i + 1 < n && (isIn(tk[(size_t)i + 1].lower, {"is", "was", "will", "'s", "morning", "evening", "night", "afternoon"}) ||
                        tk[(size_t)i + 1].text == "'s"))
        continue;
      t.upos = "ADV";
      t.feats = 0;
      changed = true;
    }
  }
  // C26: after a modal (must, can, should, will ...; "not" / "never" between) English has a bare verb: a word the
  // tagger read as an adjective or a noun that english.vpl knows as a verb is that verb ("You must not open that
  // door": "open" was an adjective and the cue a Fix)
  for (int i = 1; i < n; ++i) {
    Token& t = tk[(size_t)i];
    if (t.upos != "ADJ" && t.upos != "NOUN") continue;
    int p = i - 1;
    while (p > 0 && isIn(tk[(size_t)p].lower, {"not", "n't", "never", "always", "just", "also", "really", "only"})) --p;
    const Token& m = tk[(size_t)p];
    if (m.upos != "AUX" || !isIn(m.lower, {"must", "can", "cannot", "could", "should", "would", "will", "shall", "may",
                                           "might", "'ll", "ca", "wo", "sha"}))
      continue;
    // not the subject of an inverted question ("Can cats smile?"): a word stands before the modal and no verb follows
    if (p == 0 || isIn(tk[(size_t)p - 1].upos, {"PUNCT", "CCONJ", "SCONJ"}) ||
        isIn(tk[(size_t)p - 1].lower, {"what", "where", "when", "why", "how", "who", "which", "whom", "whose"}))
      continue;
    if (i + 1 < n && isIn(tk[(size_t)i + 1].upos, {"VERB", "AUX"})) continue;
    const Reading r = readingOf(lx, t.lower);
    if (!r.verb || !r.presentVerb) continue;
    t.upos = "VERB";
    t.feats = nlp::morph::fromString("VerbForm=Inf");
    changed = true;
  }
  return changed;
}

bool colourWord(const std::string& w) {
  return isIn(w, {"blue", "black", "white", "red", "green", "yellow", "grey", "gray", "brown", "golden", "silver",
                  "pink", "orange", "purple", "violet", "scarlet", "crimson"});   // C26: + orange ... crimson
}

}  // namespace vp::frame::en
