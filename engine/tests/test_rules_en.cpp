// engine/rules source side and engine assembly (C2): sentence mapping, phrasebook, frame builder, transfer tables,
// cue assembly, makeEngine() end to end on tests/regression/own_dialogue.en.srt (report in
// <build>/regression_report.txt), determinism, RSS. Tests needing the real data (data/work: latin.vpl, english.vpl,
// spanish.vpl, nlp/*.vpt; env VP_DATA_WORK) skip with a message when it is absent.
#include <doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "vp/cue.h"
#include "vp/curated.h"
#include "vp/engine_config.h"
#include "vp/features.h"
#include "vp/frame.h"
#include "vp/lex.h"
#include "vp/morph.h"
#include "vp/nlp.h"
#include "vp/realise_la.h"
#include "vp/rules.h"
#include "vp/subs.h"
#include "vp/text.h"
#include "vp/transfer.h"

#include "../rules/src/frame/english.h"

namespace stdfs = std::filesystem;
using namespace vp;

namespace {

stdfs::path repo() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path work() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repo() / "data" / "work";
}
stdfs::path buildDir() { return stdfs::path(VP_TEST_TMP).parent_path(); }

const curated::CuratedData& cur() {
  static Result<curated::CuratedData> r = curated::CuratedData::load(repo() / "data" / "curated");
  REQUIRE_MESSAGE(r.ok(), r.error().message);
  return r.value();
}

struct Real {
  lex::Lexicon la, en, es;
  nlp::Pipeline pen, pes;
  bool ok = false, esOk = false;
  std::string why;
};
const Real& real() {
  static Real r = [] {
    Real x;
    auto la = lex::Lexicon::open(work() / "latin.vpl");
    auto en = lex::Lexicon::open(work() / "english.vpl");
    auto pen = nlp::Pipeline::open(nlp::Lang::En, (work() / "nlp" / "english.tag.vpt").string(),
                                   (work() / "nlp" / "english.dep.vpt").string());
    if (!la.ok() || !en.ok() || !pen.ok()) {
      x.why = !la.ok() ? la.error().message : !en.ok() ? en.error().message : pen.error().message;
      return x;
    }
    x.la = std::move(la.value());
    x.en = std::move(en.value());
    x.pen = std::move(pen.value());
    x.ok = true;
    auto es = lex::Lexicon::open(work() / "spanish.vpl");
    auto pes = nlp::Pipeline::open(nlp::Lang::Es, (work() / "nlp" / "spanish.tag.vpt").string(),
                                   (work() / "nlp" / "spanish.dep.vpt").string());
    if (es.ok() && pes.ok()) {
      x.es = std::move(es.value());
      x.pes = std::move(pes.value());
      x.esOk = true;
    }
    return x;
  }();
  return r;
}
#define NEED_REAL()                                                                                  \
  if (!real().ok) {                                                                                  \
    MESSAGE("real data not available (" << real().why << "); set VP_DATA_WORK. Skipped.");          \
    return;                                                                                          \
  }

std::unique_ptr<rules::Engine> engine() {
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repo() / "data" / "curated").string();
  cfg.dataDir = work().string();
  cfg.nlpDir = (work() / "nlp").string();
  auto e = rules::makeEngine(cfg);
  const Real& r = real();
  e->setLexicons(&r.la, nullptr, &r.en, r.esOk ? &r.es : nullptr);
  return e;
}

std::string norm(const std::string& s) {
  std::string k = text::latin_key(s);
  std::string out;
  for (char c : k) {
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out += c;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::vector<std::string> splitAlt(const std::string& s) {
  std::vector<std::string> out;
  size_t a = 0;
  for (;;) {
    size_t b = s.find(" | ", a);
    out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 3;
  }
  return out;
}

const char* confName(rules::Confidence c) {
  return c == rules::Confidence::Ok ? "ok" : c == rules::Confidence::Check ? "check" : "fix";
}

long rssAnonKb() {
  std::ifstream in("/proc/self/status");
  std::string k;
  while (in >> k) {
    if (k == "RssAnon:") { long v = -1; in >> v; return v; }
    std::string rest;
    std::getline(in, rest);
  }
  return -1;
}

std::vector<rules::CueInput> regressionCues(int repeat = 1, const char* file = "own_dialogue.en.srt") {
  std::ifstream f(repo() / "tests" / "regression" / file, std::ios::binary);
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
  auto d = subs::parse(b, subs::Format::Srt);
  REQUIRE(d.ok());
  std::vector<rules::CueInput> in;
  for (int r = 0; r < repeat; ++r)
    for (size_t i = 0; i < d->cues.size(); ++i) {
      rules::CueInput c;
      c.index = (uint32_t)in.size();
      c.sourceText = d->cues[i].plainText();
      int64_t s = 0, t = 0;
      subs::parseTiming(d->cues[i].timingRaw, subs::Format::Srt, s, t);
      c.startMs = s;
      c.endMs = t;
      in.push_back(c);
    }
  return in;
}

std::string flat(std::string s) {
  std::replace(s.begin(), s.end(), '\n', ' ');
  return s;
}

}  // namespace

// ================================================================================================================
TEST_CASE("rules-en: cue -> sentence mapping (continuations, dashes, songs, nonverbal, abbreviations)") {
  using frame::CueKind;
  auto s = frame::mapSentences({"Where are you going?", "I was walking", "to the river.", "- Who is there? - Me.",
                                "\xE2\x99\xAA The river runs to the sea \xE2\x99\xAA", "[laughs]", "Mr. Smith is here.",
                                "I think...", "...that it is late.", "Wait! [door opens] Come in.", ""});
  REQUIRE(s.size() == 11);
  CHECK(s[0].text == "Where are you going?");
  CHECK(s[1].text == "I was walking to the river.");
  REQUIRE(s[1].parts.size() == 2);
  CHECK(s[1].parts[0].cue == 1);
  CHECK(s[1].parts[1].cue == 2);
  CHECK(s[1].parts[1].start == (int)std::string("I was walking ").size());
  CHECK(s[2].text == "Who is there?");
  CHECK(s[2].dash);
  CHECK(s[3].text == "Me.");
  CHECK(s[3].dash);
  CHECK(s[4].kind == CueKind::Song);
  CHECK(s[4].text == "The river runs to the sea");
  CHECK(s[4].prefix == "\xE2\x99\xAA ");
  CHECK(s[5].kind == CueKind::Nonverbal);
  CHECK(s[5].text == "laughs");
  CHECK(s[5].prefix == "[");
  CHECK(s[6].text == "Mr. Smith is here.");
  CHECK(s[7].text == "I think... ...that it is late.");
  CHECK(s[7].parts.size() == 2);
  CHECK(s[8].text == "Wait!");
  CHECK(s[9].kind == CueKind::Nonverbal);
  CHECK(s[9].text == "door opens");
  CHECK(s[10].text == "Come in.");
  // "Come in." after the bracket group is its own sentence of cue 9
  auto t = frame::mapSentences({"Wait! [door opens] Come in."});
  REQUIRE(t.size() == 3);
  CHECK(t[2].text == "Come in.");
  CHECK(frame::endsSentence("Hello!\"") == true);
  CHECK(frame::endsSentence("Hello,") == false);
}

TEST_CASE("rules-en: cue assembly splits at boundaries and never changes the cue count") {
  // a Latin sentence of 6 tokens over two source cues
  frame::SourceSentence src;
  src.text = "The girl who loves roses walks in the garden.";
  src.parts = {{0, 0, 24}, {1, 25, (int)src.text.size()}};
  cue::Latin la;
  la.text = "Puella quae rosās amat in hortō ambulat.";
  const char* words[] = {"Puella", "quae", "rosās", "amat", "in", "hortō", "ambulat"};
  const char* pos[] = {"noun", "pron", "noun", "verb", "prep", "noun", "verb"};
  const char* cs[] = {"nominative", "nominative", "accusative", "", "", "ablative", ""};
  size_t at = 0;
  for (int i = 0; i < 7; ++i) {
    rules::TokenView t;
    t.text = words[i];
    at = la.text.find(t.text, at);
    t.start = (int)at;
    t.end = (int)(at + t.text.size());
    at = (size_t)t.end;
    t.features.pos = pos[i];
    t.features.case_ = cs[i];
    t.features.number = "singular";
    la.tokens.push_back(t);
  }
  auto pieces = cue::splitSentence(src, la);
  REQUIRE(pieces.size() == 2);
  CHECK(pieces[0].text == "Puella quae rosās amat");
  CHECK(pieces[1].text == "in hortō ambulat.");
  CHECK(pieces[1].tokens.front().start == 0);
  // never inside an NP: "in | hortō" and "hortō ambulat" with a share pointing between in and hortō
  src.parts[1].start = 35;
  pieces = cue::splitSentence(src, la);
  CHECK(pieces[1].text.rfind("hortō", 0) != 0);
  // many cue parts, few words: still one piece per part
  frame::SourceSentence s3;
  s3.text = "Yes, yes, yes.";
  s3.parts = {{0, 0, 4}, {1, 5, 9}, {2, 10, 14}};
  cue::Latin one;
  one.text = "Ita.";
  rules::TokenView t;
  t.text = "Ita";
  t.end = 3;
  one.tokens.push_back(t);
  CHECK(cue::splitSentence(s3, one).size() == 3);
  // layout
  auto lay = cue::layout("Puella quae rosās amat et fēlēs quae in hortō dormit nunc ambulant ad flūmen magnum.", 42, 2);
  CHECK(lay.lines.size() == 2);
  CHECK(lay.joined.find('\n') != std::string::npos);
  std::vector<rules::TokenView> toks = la.tokens;
  cue::relocate("Puella quae rosās\namat in hortō ambulat.", toks);
  CHECK(toks[3].start == (int)std::string("Puella quae rosās\n").size());
}

TEST_CASE("rules-en: tag policy and nonverbal table") {
  auto spans = subs::splitSpans("<i>Where are you?</i>", subs::Format::Srt);
  auto r = cue::applyTags(spans, "Ubi es?");
  REQUIRE(r.spans.size() == 3);
  CHECK(r.spans[0].raw == "<i>");
  CHECK(r.spans[1].raw == "Ubi es?");
  CHECK(r.spans[2].raw == "</i>");
  CHECK(!r.approximated);
  auto partial = cue::applyTags(subs::splitSpans("Where <i>are</i> you?", subs::Format::Srt), "Ubi es?");
  CHECK(partial.approximated);
  CHECK(partial.spans.size() == 1);
  auto pos = cue::applyTags(subs::splitSpans("{\\an8}Hello", subs::Format::Ass), "Salvē");
  CHECK(pos.spans.size() == 2);
  CHECK(!pos.approximated);
  bool tr = false;
  CHECK(cue::nonverbal("laughs", cur(), tr) == "rīdet");
  CHECK(tr);
  CHECK(cue::nonverbal("helicopter noise", cur(), tr) == "helicopter noise");
  CHECK(!tr);
}

TEST_CASE("rules-en: phrasebook matching (slots, optional tokens, alternatives, contractions)") {
  frame::Phrasebook pb;
  std::vector<curated::PhraseEntry> rows = {{"see you (soon|later)", "mox tē vidēbō", "greet", "", 1},
                                            {"i'm late", "sērō veniō", "state", "", 1},
                                            {"where is {NP}", "ubi est {1:nom}", "q", "", 1},
                                            {"my name is {NAME}", "nōmen mihi est {1}", "intro", "", 1},
                                            {"it's time to {VP}", "tempus est {1:inf}", "state", "", 1},
                                            {"thank you", "grātiās agō", "polite", "", 1},
                                            {"thank you very much", "grātiās maximās agō", "polite", "", 1},
                                            {"hi|hello", "salvē", "greet", "", 1}};
  pb.build(rows, cur().contractions());
  auto toks = [](std::initializer_list<std::pair<const char*, const char*>> w) {
    std::vector<nlp::Token> v;
    for (auto& x : w) {
      nlp::Token t;
      t.text = x.first;
      t.lower = text::lower(x.first);
      t.upos = x.second;
      t.lemma = t.lower;
      v.push_back(t);
    }
    return v;
  };
  frame::PhraseMatch m;
  REQUIRE(pb.match(toks({{"See", "VERB"}, {"you", "PRON"}, {"later", "ADV"}, {".", "PUNCT"}}), 0, m));
  CHECK(m.last == 2);
  REQUIRE(pb.match(toks({{"See", "VERB"}, {"you", "PRON"}, {".", "PUNCT"}}), 0, m));   // optional token absent
  CHECK(m.last == 1);
  REQUIRE(pb.match(toks({{"I", "PRON"}, {"am", "AUX"}, {"late", "ADV"}}), 0, m));       // contraction in the pattern
  CHECK(cur().phrasebook().size() > 0);
  auto w = toks({{"Where", "ADV"}, {"is", "AUX"}, {"the", "DET"}, {"old", "ADJ"}, {"ball", "NOUN"}, {"?", "PUNCT"}});
  REQUIRE(pb.match(w, 0, m));
  REQUIRE(m.slots.size() == 1);
  CHECK(m.slots[0].first == 2);
  CHECK(m.slots[0].last == 4);
  REQUIRE(pb.match(toks({{"My", "PRON"}, {"name", "NOUN"}, {"is", "AUX"}, {"Alice", "PROPN"}}), 0, m));
  CHECK(m.slots[0].kind == frame::SlotKind::Name);
  REQUIRE(pb.match(toks({{"It", "PRON"}, {"is", "AUX"}, {"time", "NOUN"}, {"to", "PART"}, {"go", "VERB"}, {"home", "ADV"}}), 0, m));
  CHECK(m.slots[0].kind == frame::SlotKind::VP);
  CHECK(m.slots[0].last == 5);
  REQUIRE(pb.match(toks({{"Thank", "VERB"}, {"you", "PRON"}, {"very", "ADV"}, {"much", "ADV"}}), 0, m));
  CHECK(m.last == 3);   // longest match wins
  CHECK(pb.match(toks({{"Hello", "INTJ"}}), 0, m));
  CHECK(!pb.match(toks({{"Goodbye", "INTJ"}}), 0, m));
}

TEST_CASE("rules-en: frame builder on 40 own sentences (tests/fixtures/rules_en/frames_en.tsv)") {
  NEED_REAL();
  frame::FrameBuilder fb(frame::SrcLang::En, &real().pen, &real().en, cur());
  std::ifstream in(stdfs::path(VP_FIXTURES_DIR) / "rules_en" / "frames_en.tsv");
  std::string line;
  int rows = 0, good = 0;
  frame::SemSentence s;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const size_t tab = line.find('\t');
    REQUIRE(tab != std::string::npos);
    const std::string sent = line.substr(0, tab);
    fb.analyse(sent, s);
    ++rows;
    const frame::SemFrame* f = nullptr;
    const frame::PhraseMatch* ph = nullptr;
    for (const frame::Unit& u : s.units) {
      if (u.type == frame::Unit::Clause && !f && !u.vocative) f = &u.frame;
      if (u.type == frame::Unit::Phrase && !ph) ph = &u.phrase;
    }
    auto npName = [](const frame::SemNP& n) { return n.isPronoun ? n.pronLemma : n.head; };
    std::map<std::string, std::string> got;
    got["units"] = std::to_string(s.units.size());
    if (ph) got["phrase"] = ph->pattern;
    if (f) {
      got["kind"] = frame::kindName(f->type);
      got["pred"] = f->pred.lemma;
      got["tense"] = frame::tenseName(f->pred.tense);
      got["aspect"] = frame::aspectName(f->pred.aspect);
      got["mod"] = frame::modalityName(f->pred.modality);
      got["neg"] = f->negative ? "1" : "0";
      got["exist"] = f->existential ? "1" : "0";
      got["passive"] = f->pred.voice == frame::Voice::Passive ? "1" : "0";
      if (f->hasSubject) got["subj"] = npName(f->subject);
      if (f->hasObject) got["obj"] = npName(f->object);
      if (f->hasIndirect) got["iobj"] = npName(f->indirectObject);
      if (!f->subordinate.empty())
        got["sub"] = std::string(frame::relationName(f->subordinate[0].relation)) + ":" + f->subordinate[0].marker;
      if (f->type == frame::Kind::Wh) got["wh"] = f->wh.word + ":" + frame::roleName(f->wh.role);
      if (!f->obliques.empty())
        got["obl"] = (f->obliques[0].prep.empty() ? std::string("-") : f->obliques[0].prep) + ":" + f->obliques[0].np.head;
    }
    std::istringstream ex(line.substr(tab + 1));
    std::string kv;
    bool all = true;
    // values may contain spaces only for "phrase=": read the rest of the line for it
    std::string rest = line.substr(tab + 1);
    std::vector<std::pair<std::string, std::string>> exp;
    {
      size_t p = 0;
      while (p < rest.size()) {
        while (p < rest.size() && rest[p] == ' ') ++p;
        const size_t eq = rest.find('=', p);
        if (eq == std::string::npos) break;
        const std::string k = rest.substr(p, eq - p);
        size_t end = rest.find(' ', eq + 1);
        // a value runs to the next " key=" (phrase patterns contain spaces)
        size_t next = eq + 1;
        for (;;) {
          end = rest.find(' ', next);
          if (end == std::string::npos) break;
          const size_t eq2 = rest.find('=', end);
          const size_t sp2 = rest.find(' ', end + 1);
          if (eq2 != std::string::npos && (sp2 == std::string::npos || eq2 < sp2)) break;
          next = end + 1;
        }
        exp.emplace_back(k, rest.substr(eq + 1, end == std::string::npos ? std::string::npos : end - eq - 1));
        p = end == std::string::npos ? rest.size() : end + 1;
      }
    }
    for (const auto& e : exp) {
      const bool ok = got.count(e.first) && got[e.first] == e.second;
      CHECK_MESSAGE(ok, sent << ": " << e.first << " expected '" << e.second << "' got '"
                              << (got.count(e.first) ? got[e.first] : std::string("<none>")) << "'  ["
                              << frame::describe(s) << "]");
      all = all && ok;
    }
    good += all;
  }
  CHECK(rows == 40);
  MESSAGE("frame builder: " << good << " / " << rows << " sentences fully as expected");
}

TEST_CASE("rules-en: Spanish source frames (clitics, usted, ¿?, subjunctive by marker)") {
  NEED_REAL();
  if (!real().esOk) { MESSAGE("Spanish models / lexicon not available; skipped"); return; }
  frame::FrameBuilder fb(frame::SrcLang::Es, &real().pes, &real().es, cur());
  frame::SemSentence s;
  auto first = [&]() -> const frame::SemFrame& {
    for (const frame::Unit& u : s.units)
      if (u.type == frame::Unit::Clause) return u.frame;
    static frame::SemFrame none;
    return none;
  };
  fb.analyse("¿Dónde duerme el gato?", s);   // "¿Dónde está ...?" is a phrasebook row since C13
  CHECK(s.question);
  CHECK(first().type == frame::Kind::Wh);
  CHECK(first().wh.word == "where");
  fb.analyse("No conozco a tu hermano.", s);
  CHECK(first().negative);
  CHECK(first().hasSubject);   // pro-drop subject from the verb
  CHECK(first().subject.pron.person == 1);
  fb.analyse("Dámelo.", s);
  bool clitic = false;
  for (const nlp::Token& t : s.tokens) clitic = clitic || t.lower == "lo";
  CHECK(clitic);   // enclitics split from the imperative
  fb.analyse("Usted es muy amable.", s);
  CHECK(first().subject.isPronoun);
  CHECK(first().subject.pron.person == 2);
  fb.analyse("Vamos al jardín.", s);
  bool al = false;
  for (const nlp::Token& t : s.tokens) al = al || t.lower == "el";
  CHECK(al);   // al -> a el
  MESSAGE("es: " << frame::describe(s));
}

TEST_CASE("rules-en: transfer tables (pronouns, determiners, prepositions, negation, wh)") {
  NEED_REAL();
  const lex::Lexicon& la = real().la;
  transfer::Transfer tr(la, cur());
  transfer::Settings st;
  transfer::Memory mem;
  frame::SemSentence s;
  auto head = [&](uint32_t id) { return id == lex::kNoLemma ? std::string("-") : std::string(la.lemma(id).head); };
  // pronouns
  frame::SemNP p;
  p.isPronoun = true;
  p.pronLemma = "we";
  p.pron.person = 1;
  p.pron.number = 2;
  transfer::ClauseOut out;
  realise::LaNP x = tr.np(p, s, st, mem, out);
  CHECK(x.isPronoun);
  CHECK(x.pron.person == 1);
  CHECK(x.pron.number == feat::Pl);
  p.pronLemma = "nobody";
  p.pron = frame::SemPronoun{3, 1, 0, false, false};
  CHECK(head(tr.np(p, s, st, mem, out).head) == "nēmō");
  p.pronLemma = "everyone";
  x = tr.np(p, s, st, mem, out);
  CHECK(head(x.head) == "omnis");
  CHECK(x.number == feat::Pl);
  p.pronLemma = "this";
  CHECK(head(tr.np(p, s, st, mem, out).head) == "hic");
  p.pronLemma = "i";
  p.pron = frame::SemPronoun{1, 1, 0, false, false};
  st.speakerGender = 'f';
  CHECK(tr.np(p, s, st, mem, out).pron.gender == feat::F);
  st.speakerGender = 'm';
  // determiners and possessives
  frame::SemNP n;
  n.head = "girl";
  n.determiner = "this";
  x = tr.np(n, s, st, mem, out);
  CHECK(head(x.head) == "puella");
  CHECK(x.det == realise::Det::Hic);
  n.determiner = "that";
  CHECK(tr.np(n, s, st, mem, out).det == realise::Det::Ille);
  n.determiner = "the";
  CHECK(tr.np(n, s, st, mem, out).det == realise::Det::None);
  n.determiner = "no";
  x = tr.np(n, s, st, mem, out);
  REQUIRE(!x.adjectives.empty());
  CHECK(head(x.adjectives[0].lemma) == "nūllus");
  n.determiner = "every";
  CHECK(head(tr.np(n, s, st, mem, out).adjectives[0].lemma) == "omnis");
  n.determiner.clear();
  frame::SemNP my;
  my.isPronoun = true;
  my.pronLemma = "my";
  my.pron.person = 1;
  my.pron.number = 1;
  n.possessor.push_back(my);
  CHECK(head(tr.np(n, s, st, mem, out).possessive) == "meus");
  n.possessor[0].pron.person = 3;
  n.possessor[0].pronLemma = "his";
  x = tr.np(n, s, st, mem, out);
  REQUIRE(x.genitive.size() == 1);   // non-reflexive his -> eius
  CHECK(x.genitive[0].isPronoun);
  n.possessor.clear();
  n.numeral = "two";
  n.numeralValue = 2;
  CHECK(head(tr.np(n, s, st, mem, out).numeral) == "duo");
  n.numeral.clear();
  // prepositions and negation through a clause
  auto clauseWith = [&](const std::string& prep, const std::string& noun, const std::string& verb) {
    frame::SemFrame f;
    f.hasPred = true;
    f.pred.lemma = verb;
    frame::SemOblique o;
    o.prep = prep;
    o.np.head = noun;
    if (noun == "me") { o.np.isPronoun = true; o.np.pronLemma = "me"; o.np.pron.person = 1; o.np.pron.number = 1; }
    f.obliques.push_back(o);
    transfer::ClauseOut co;
    tr.clause(f, s, st, mem, co);
    return co.clause;
  };
  auto cl = clauseWith("in", "garden", "sit");
  REQUIRE(cl.obliques.size() == 1);
  CHECK(head(cl.obliques[0].prep) == "in");
  CHECK(cl.obliques[0].case_ == feat::Abl);
  cl = clauseWith("into", "garden", "run");
  CHECK(cl.obliques[0].case_ == feat::Acc);
  cl = clauseWith("to", "river", "walk");
  CHECK(head(cl.obliques[0].prep) == "ad");
  cl = clauseWith("to", "me", "speak");
  CHECK(cl.hasIndirect);   // recipient -> dative
  cl = clauseWith("with", "sword", "fight");
  CHECK(cl.obliques[0].prep == lex::kNoLemma);   // instrument: bare ablative
  CHECK(cl.obliques[0].case_ == feat::Abl);
  cl = clauseWith("with", "friend", "walk");
  CHECK(head(cl.obliques[0].prep) == "cum");
  cl = clauseWith("without", "friend", "walk");
  CHECK(head(cl.obliques[0].prep) == "sine");
  cl = clauseWith("for", "me", "wait");
  CHECK(cl.hasObject);   // wait for X -> exspectāre X
  CHECK(head(cl.pred.lemma) == "exspectō");
  // negation and wh
  frame::SemFrame f;
  f.hasPred = true;
  f.pred.lemma = "know";
  f.negative = true;
  f.type = frame::Kind::Wh;
  f.wh.word = "why";
  f.wh.role = frame::Role::Adverb;
  transfer::ClauseOut co;
  tr.clause(f, s, st, mem, co);
  // C15: "not know" is nesciō (the negation goes into the verb): "Why don't you know?" -> Cūr nescīs?
  CHECK(co.clause.polarity == realise::Polarity::Pos);
  CHECK(head(co.clause.wh.lemma) == "cūr");
  CHECK(head(co.clause.pred.lemma) == "nesciō");
  // modality and tense mapping
  f = frame::SemFrame{};
  f.hasPred = true;
  f.pred.lemma = "go";
  f.pred.modality = frame::Modality::Can;
  f.pred.tense = frame::Tense::Past;
  f.pred.pastModal = true;
  tr.clause(f, s, st, mem, co);
  CHECK(head(co.clause.pred.modal) == "possum");
  CHECK(co.clause.pred.tense == feat::Imperfect);
  f.pred = frame::SemPredicate{};
  f.pred.lemma = "open";
  f.pred.tense = frame::Tense::Past;
  tr.clause(f, s, st, mem, co);
  CHECK(co.clause.pred.tense == feat::Perfect);   // event in the past
  f.pred.aspect = frame::Aspect::Perfect;
  tr.clause(f, s, st, mem, co);
  CHECK(co.clause.pred.tense == feat::Pluperfect);
  f.pred.lemma = "live";
  f.pred.aspect = frame::Aspect::Simple;
  tr.clause(f, s, st, mem, co);
  CHECK(co.clause.pred.tense == feat::Imperfect);   // state in the past
  // unknown word: no guess
  transfer::Choice ch;
  CHECK(tr.select("zxqvword", feat::Noun, {}, false, false, st, ch) == lex::kNoLemma);
  CHECK(ch.unknown);
  // fidelity: tier penalty at 3 prefers tier 1
  st.fidelity = 3;
  tr.select("big", feat::Adj, {}, false, false, st, ch);
  REQUIRE(!ch.candidates.empty());
  CHECK(head(ch.lemma) == "magnus");
}

TEST_CASE("rules-en: engine boundary (lexicon missing, check, inspect, reasons, cancel, progress)") {
  {
    auto e = rules::makeEngine(rules::defaultEngineConfig());
    e->setLexicons(nullptr, nullptr, nullptr, nullptr);
    rules::CueInput c;
    c.sourceText = "Hello.";
    auto r = e->translate({c}, rules::Options{}, rules::Context{}, nullptr, nullptr);
    CHECK(!r.ok());
    CHECK(r.error().code == ErrorCode::LexiconMissing);
    CHECK(!r.error().hint.empty());
    CHECK(e->inspect("amō", rules::Lang::La, rules::Options{}).error().code == ErrorCode::LexiconMissing);
    CHECK(e->version() == "rules-1");
  }
  NEED_REAL();
  auto e = engine();
  rules::Options o;
  rules::Context ctx;
  // check(): an edited cue
  rules::CueInput ci;
  ci.startMs = 0;
  ci.endMs = 2000;
  auto ck = e->check(ci, "Puella rosās amat.", o, ctx);
  REQUIRE(ck.ok());
  CHECK(ck->confidence == rules::Confidence::Ok);
  CHECK(ck->tokens.size() == 3);
  auto bad = e->check(ci, "Puella rosās amant.", o, ctx);
  REQUIRE(bad.ok());
  CHECK(bad->confidence == rules::Confidence::Fix);
  bool hasA9 = false;
  for (const auto& c : bad->checks) hasA9 = hasA9 || (c.id == "A9" && c.detail.find("not implemented") != std::string::npos);
  CHECK(hasA9);
  // inspect
  auto la = e->inspect("amat", rules::Lang::La, o);
  REQUIRE(la.ok());
  REQUIRE(!la->analyses.empty());
  CHECK(text::latin_key(la->analyses[0].head) == "amo");
  auto en = e->inspect("loves", rules::Lang::En, o);
  REQUIRE(en.ok());
  bool amo = false;
  for (const auto& a : en->analyses) amo = amo || a.head == "amō";
  CHECK(amo);   // the English lemma's Latin candidates
  // reasons, model/online off, alternatives
  o.useModel = true;
  o.useOnline = true;
  rules::CueInput c1;
  c1.sourceText = "The queen sees the white roses.";
  c1.endMs = 3000;
  auto r = e->translate({c1}, o, ctx, nullptr, nullptr);
  REQUIRE(r.ok());
  REQUIRE(r->size() == 1);
  const rules::CueOutput& out = r.value()[0];
  CHECK(flat(out.target) == "Rēgīna rosās albās videt.");
  bool modelOff = false, onlineOff = false, sense = false, cand = false;
  for (const auto& rs : out.reasons) {
    modelOff = modelOff || rs.text == "model: off";
    onlineOff = onlineOff || rs.text == "online: off";
    sense = sense || rs.kind == "sense";
    cand = cand || (rs.kind == "candidate" && rs.data.find("\"head\"") != std::string::npos);
  }
  CHECK(modelOff);
  CHECK(onlineOff);
  CHECK(sense);
  CHECK(cand);
  for (const auto& t : out.tokens) {
    REQUIRE(t.start >= 0);
    CHECK(out.target.substr((size_t)t.start, (size_t)(t.end - t.start)) == t.text);
  }
  // macrons off: same words without length marks
  rules::Options nm;
  nm.macrons = false;
  auto r2 = e->translate({c1}, nm, ctx, nullptr, nullptr);
  REQUIRE(r2.ok());
  CHECK(flat(r2.value()[0].target) == "Regina rosas albas videt.");
  // speaker gender unknown: masculine form, feminine alternative, Check
  rules::Options ug;
  ug.speakerGender = 'u';
  rules::CueInput c2;
  c2.sourceText = "I am tired.";
  auto r3 = e->translate({c2}, ug, ctx, nullptr, nullptr);
  REQUIRE(r3.ok());
  CHECK(r3.value()[0].confidence != rules::Confidence::Ok);
  bool fem = false;
  for (const auto& a : r3.value()[0].alternatives) fem = fem || a.text.find("essa") != std::string::npos;
  CHECK(fem);
  // cancel at cue granularity and progress
  std::vector<rules::CueInput> many = regressionCues();
  size_t last = 0;
  bool monotone = true;
  int calls = 0;
  auto r4 = e->translate(many, rules::Options{}, ctx, [&](size_t d) { monotone = monotone && d >= last; last = d; },
                         [&]() { return ++calls > 30; });
  REQUIRE(r4.ok());
  CHECK(r4->size() < many.size());
  CHECK(r4->size() >= 20);
  CHECK(monotone);
  // a sentence across two cues: one translation, two pieces, cue count unchanged
  rules::CueInput a, b;
  a.sourceText = "The little girl walks";
  b.sourceText = "in the garden with her cat.";
  b.index = 1;
  auto r5 = e->translate({a, b}, rules::Options{}, ctx, nullptr, nullptr);
  REQUIRE(r5.ok());
  REQUIRE(r5->size() == 2);
  CHECK(!r5.value()[0].target.empty());
  CHECK(!r5.value()[1].target.empty());
  MESSAGE("split: [" << flat(r5.value()[0].target) << "] [" << flat(r5.value()[1].target) << "]");
  // the same pair in two batches (prevSource / nextSource) gives the same pieces
  rules::CueInput a2 = a, b2 = b;
  a2.nextSource = b.sourceText;
  b2.prevSource = a.sourceText;
  auto ra = e->translate({a2}, rules::Options{}, ctx, nullptr, nullptr);
  auto rb = e->translate({b2}, rules::Options{}, ctx, nullptr, nullptr);
  REQUIRE(ra.ok());
  REQUIRE(rb.ok());
  CHECK(ra.value()[0].target == r5.value()[0].target);
  CHECK(rb.value()[0].target == r5.value()[1].target);
  // a remembered whole-cue correction replaces the translation (key = en_key of the cue source, as the CLI stores it)
  rules::Context cc;
  cc.corrections.push_back(rules::Correction{text::en_key(c1.sourceText), "Rēgīna rosās candidās videt.", "cue", 2});
  auto rc = e->translate({c1}, rules::Options{}, cc, nullptr, nullptr);
  REQUIRE(rc.ok());
  CHECK(flat(rc.value()[0].target) == "Rēgīna rosās candidās videt.");
  bool corrReason = false;
  for (const auto& rs : rc.value()[0].reasons) corrReason = corrReason || rs.kind == "correction";
  CHECK(corrReason);
  // engine ii / iii hooks: a closed choice re-ranks the most ambiguous word; online disagreement lowers to Check
  {
    rules::EngineConfig cfg = rules::defaultEngineConfig();
    cfg.curatedDir = (repo() / "data" / "curated").string();
    cfg.nlpDir = (work() / "nlp").string();
    int asked = 0;
    cfg.advisors.chooseSense = [&](const std::string&, const std::vector<std::string>& opts) {
      ++asked;
      return opts.size() > 1 ? 1 : 0;
    };
    cfg.advisors.onlineCheck = [](const std::string&, const std::string&) {
      return rules::Evidence{-1, "test", "disagrees"};
    };
    auto em = rules::makeEngine(cfg);
    em->setLexicons(&real().la, nullptr, &real().en, nullptr);
    rules::Options mo;
    mo.useModel = true;
    mo.useOnline = true;
    rules::CueInput cm;
    cm.sourceText = "The girl sings a song.";
    auto rm = em->translate({cm}, mo, ctx, nullptr, nullptr);
    REQUIRE(rm.ok());
    CHECK(asked == 1);
    bool chose = false;
    for (const auto& rs : rm.value()[0].reasons) chose = chose || rs.text.rfind("model: chose", 0) == 0;
    CHECK(chose);
    CHECK(!rm.value()[0].alternatives.empty());
    CHECK(rm.value()[0].confidence != rules::Confidence::Ok);
  }
  // nonverbal and song cues
  rules::CueInput nv, sg;
  nv.sourceText = "[laughs]";
  sg.sourceText = "\xE2\x99\xAA The cat sings \xE2\x99\xAA";
  auto r6 = e->translate({nv, sg}, rules::Options{}, ctx, nullptr, nullptr);
  REQUIRE(r6.ok());
  CHECK(r6.value()[0].target == "[rīdet]");
  CHECK(r6.value()[0].confidence == rules::Confidence::Check);
  CHECK(r6.value()[1].target.rfind("\xE2\x99\xAA ", 0) == 0);
  CHECK(std::find(r6.value()[1].flags.begin(), r6.value()[1].flags.end(), "song") != r6.value()[1].flags.end());
}

TEST_CASE("rules-en: end to end on own_dialogue.en.srt vs the gold Latin (report; determinism; coverage)") {
  NEED_REAL();
  auto e = engine();
  std::vector<rules::CueInput> in = regressionCues();
  REQUIRE(in.size() == 114);
  rules::Options o;
  o.fidelity = 2;
  o.speakerGender = 'f';   // the gold file's speaker is Alice (header of the gold file)
  rules::Context ctx;
  auto r1 = e->translate(in, o, ctx, nullptr, nullptr);
  REQUIRE(r1.ok());
  auto r2 = engine()->translate(in, o, ctx, nullptr, nullptr);   // a fresh engine: byte-identical
  REQUIRE(r2.ok());
  REQUIRE(r1->size() == 114);
  REQUIRE(r2->size() == 114);
  bool same = true;
  for (size_t i = 0; i < 114; ++i) {
    same = same && r1.value()[i].target == r2.value()[i].target;
    same = same && r1.value()[i].confidence == r2.value()[i].confidence;
  }
  CHECK(same);
  // gold
  std::ifstream g(repo() / "tests" / "regression" / "expected" / "own_dialogue.la.gold.txt");
  std::vector<std::string> gold;
  std::string line;
  while (std::getline(g, line))
    if (!line.empty() && line[0] != '#') gold.push_back(line);
  REQUIRE(gold.size() == 114);
  // cues whose source content words all have a tier-1/2 Latin candidate: no brackets allowed in the output
  frame::FrameBuilder fb(frame::SrcLang::En, &real().pen, &real().en, cur());
  transfer::Transfer tr(real().la, cur());
  transfer::Settings st;
  int matches = 0, t12 = 0, t12clean = 0, exact = 0;
  std::map<std::string, int> conf, checkWhy;
  std::ostringstream table;
  table << "| # | source | gold | ours | checks |\n|---|---|---|---|---|\n";
  int shown = 0;
  for (size_t i = 0; i < 114; ++i) {
    const rules::CueOutput& c = r1.value()[i];
    CHECK_MESSAGE(!c.target.empty(), "empty target for cue " << i + 1);
    ++conf[confName(c.confidence)];
    const std::string ours = flat(c.target);
    bool match = false, exactMatch = false;
    for (const std::string& alt : splitAlt(gold[i])) {
      match = match || norm(alt) == norm(ours);
      exactMatch = exactMatch || text::nfc(alt) == text::nfc(ours);
    }
    matches += match;
    exact += exactMatch;
    if (c.confidence == rules::Confidence::Check) {   // why Check (C2b): flags and failed checks, else the margin
      bool why = false;
      for (const char* f : {"frame-fallback", "addressee-guess", "low-tier", "speaker-gender", "name-guessed",
                            "from-rule", "missing-form", "merged", "tags-approximated", "song", "nonverbal"})
        if (std::find(c.flags.begin(), c.flags.end(), f) != c.flags.end()) { ++checkWhy[f]; why = true; }
      for (const auto& k : c.checks)
        if (!k.ok) { ++checkWhy[k.id]; why = true; }
      if (!why) ++checkWhy["margin < 0.15"];
    }
    frame::SemSentence s;
    bool allT12 = true;
    for (const auto& ss : frame::mapSentences({in[i].sourceText})) {
      fb.analyse(ss.text, s);
      for (size_t k = 0; k < s.tokens.size(); ++k) {
        const nlp::Token& t = s.tokens[k];
        uint8_t pos = t.upos == "NOUN" ? feat::Noun : t.upos == "VERB" ? feat::Verb : t.upos == "ADJ" ? feat::Adj : 0;
        if (!pos || s.drop[k] != frame::Drop::No) continue;
        transfer::Choice ch;
        tr.select(t.lemma, pos, {}, false, false, st, ch);
        bool ok = false;
        for (const auto& cd : ch.candidates) {
          const uint8_t tier = real().la.lemma(cd.lemma).tier;
          ok = ok || (tier == 1 || tier == 2);
        }
        allT12 = allT12 && ok;
      }
    }
    if (allT12) {
      ++t12;
      const bool clean = ours.find('[') == std::string::npos;
      t12clean += clean;
      CHECK_MESSAGE(clean, "cue " << i + 1 << " has T1/T2 candidates for every word but brackets: " << ours);
    }
    if (!match && shown < 40) {
      ++shown;
      std::string chk;
      for (const auto& k : c.checks)
        if (!k.ok) chk += k.id + " ";
      std::string g2 = gold[i];
      for (size_t at; (at = g2.find(" | ")) != std::string::npos;) g2.replace(at, 3, " / ");
      table << "| " << i + 1 << " | " << in[i].sourceText << " | " << g2 << " | " << ours << " | "
            << confName(c.confidence) << (chk.empty() ? "" : " " + chk) << "|\n";
    }
  }
  std::ostringstream rep;
  rep << "Regression own_dialogue.en.srt -> Latin, fidelity 2, speaker f\n";
  rep << "match rate (normalised: NFC, macron/punctuation/case-insensitive, any gold alternative): " << matches
      << " / 114\n";
  rep << "exact (macrons and punctuation too): " << exact << " / 114\n";
  rep << "confidence: ok " << conf["ok"] << ", check " << conf["check"] << ", fix " << conf["fix"] << "\n";
  rep << "check because:";
  for (const auto& w : checkWhy) rep << " " << w.first << " " << w.second << ";";
  rep << "\n";
  rep << "cues with T1/T2 candidates for every content word: " << t12 << " (bracket-free: " << t12clean << ")\n\n";
  rep << "First " << shown << " mismatches:\n" << table.str() << "\nAll outputs:\n";
  for (size_t i = 0; i < 114; ++i)
    rep << i + 1 << "\t" << in[i].sourceText << "\t" << flat(r1.value()[i].target) << "\t"
        << confName(r1.value()[i].confidence) << "\n";
  std::ofstream(buildDir() / "regression_report.txt") << rep.str();
  MESSAGE("regression: " << matches << " / 114 match the gold; confidence ok " << conf["ok"] << " / check "
                         << conf["check"] << " / fix " << conf["fix"] << "; T1/T2 cues " << t12 << " (clean "
                         << t12clean << "); report " << (buildDir() / "regression_report.txt").string());
}

// ================================================================================================================
// C2b (quality loop 1): curated tables, lexical selection, constructions, fallbacks, "you" number, tags, confidence.

TEST_CASE("rules-c: curated loaders take the new tables and odd rows (warnings, never a failed load)") {
  const curated::CuratedData& d = cur();
  // the shipped tables
  const curated::PhrasalEntry* ph = d.phrasal("go", "away");
  REQUIRE(ph != nullptr);
  CHECK(ph->latin == "abeō");
  REQUIRE(d.phrasal("bow", "-") != nullptr);
  CHECK(d.phrasal("bow", "-")->frame == "refl");
  const curated::VerbPrepEntry* vp = d.verbPrep("depend", "on");
  REQUIRE(vp != nullptr);
  CHECK(vp->latinPrep == "ex");
  CHECK(vp->prepCase == feat::Abl);
  REQUIRE(d.verbPrep("help", "with") != nullptr);
  CHECK(d.verbPrep("wait", "for")->frame == "obj");
  REQUIRE(d.state("afraid") != nullptr);
  CHECK(d.state("afraid")->kind == "verb");
  CHECK(d.state("tired")->kind == "adj");
  CHECK(d.state("tener miedo")->latin == "timeō");
  REQUIRE(d.macronOverride("narro") != nullptr);
  CHECK(d.macronOverride("narro")->to == "nārr");
  // homograph rows: key without the digit, identity by key + part of speech
  const curated::TierEntry* sow = d.tier("sero", (uint8_t)feat::Verb);
  REQUIRE(sow != nullptr);
  CHECK(sow->homograph == 2);
  CHECK(sow->tier == 2);
  const curated::TierEntry* late = d.tier("sero", (uint8_t)feat::Adv);
  REQUIRE(late != nullptr);
  CHECK(late->tier == 1);
  CHECK(d.tier("sero", (uint8_t)feat::Noun) == nullptr);   // a homograph row of another pos does not apply
  CHECK(d.tier("eo", (uint8_t)feat::Adv)->note == "thither");
  CHECK(d.tier("eo", (uint8_t)feat::Verb)->tier == 1);
  CHECK(d.effectiveTier("horologium", feat::Noun, 3) == 2);   // the curated tier wins over the lexicon's
  CHECK(d.effectiveTier("no_such_word", feat::Noun, 0) == 3);
  // teacher glosses from the tier notes
  std::vector<const curated::TierEntry*> g;
  d.glossTiers("hole", g);
  REQUIRE(g.size() == 1);
  CHECK(g[0]->key == "fouea");
  d.glossTiers("dark", g);   // "darkness, the dark (plural)": article and parentheses dropped
  REQUIRE(g.size() == 1);
  CHECK(g[0]->key == "tenebrae");
  for (const auto& w : d.warnings()) MESSAGE("curated warning: " << w.file << ":" << w.line << " " << w.message);

  // a copy with the main agent's new row shapes and broken rows appended: warnings, never a failed load or a crash
  const stdfs::path dir = stdfs::path(VP_TEST_TMP) / "curated_c2b";
  std::error_code ec;
  stdfs::remove_all(dir, ec);
  stdfs::create_directories(dir, ec);
  for (const auto& e : stdfs::directory_iterator(repo() / "data" / "curated"))
    stdfs::copy_file(e.path(), dir / e.path().filename(), stdfs::copy_options::overwrite_existing, ec);
  {
    std::ofstream(dir / "phrasebook_en_la.tsv", std::ios::app)
        << "what a {ADJ} {NP}\tquam {1} {2:nom}\t1\texcl\t\n"
        << "bow\tinclīnāte vōs\t1\timp\tplural\n"
        << "impossible\tfierī nōn potest\t1\tstate\t\n"
        << "in latin\tLatīnē\t1\tadv\t\n"
        << "broken {FOO} slot\tx {1}\t1\tq\t\n"
        << "unbalanced (optional\tx\t1\tq\t\n"
        << "bad tier\tx\tnine\tq\t\n"
        << "no latin column\n";
    std::ofstream(dir / "tiers_la.tsv", std::ios::app)
        << "cum\tcum\tprep\t1\tderived\t\n"
        << "ne\t-ne\tparticle\t1\tderived\tquestion enclitic\n"
        << "hic\thīc\tadv\t1\tderived\there\n"
        << "eo\teō\tadv\t1\tderived\tthither\n"
        << "sero2\tserō\tverb\t2\tderived\tsow, plant (homograph of sērō adv)\n"
        << "2\tdigit only\tnoun\t1\tderived\t\n"
        << "x\tx\tnoun\tseven\tderived\t\n";
    std::ofstream(dir / "phrasal_en_la.tsv", std::ios::app) << "go\taway\tabeō\tbogus\t\nlonely\n\tx\ty\t\t\n";
    std::ofstream(dir / "verbprep_en_la.tsv", std::ios::app) << "x\ty\tz\tprep:nowhere\t\nx\ty\tz\tmaybe\t\nshort\n";
    std::ofstream(dir / "states_en_la.tsv", std::ios::app) << "glad\tlaetus\tnoun\t\nonly\n";
    std::ofstream(dir / "macron_overrides.tsv", std::ios::app) << "x\t\tnārr\t\n";
  }
  Result<curated::CuratedData> r = curated::CuratedData::load(dir);
  REQUIRE(r.ok());
  size_t warned = 0;
  for (const auto& w : r.value().warnings())
    if (w.line > 0 || w.message.find("duplicate") != std::string::npos) ++warned;
  CHECK(warned >= 9);
  CHECK(r.value().phrasal("go", "away")->latin == "abeō");   // the first row wins
  CHECK(r.value().state("glad")->kind == "adj");
  // the phrasebook compiles every odd pattern without a crash; the good ones still match
  frame::Phrasebook pb;
  pb.build(r.value().phrasebook(), r.value().contractions());
  std::vector<nlp::Token> toks(4);
  const char* w4[] = {"what", "a", "strange", "garden"};
  const char* u4[] = {"PRON", "DET", "ADJ", "NOUN"};
  for (int i = 0; i < 4; ++i) { toks[(size_t)i].text = toks[(size_t)i].lower = w4[i]; toks[(size_t)i].upos = u4[i]; }
  frame::PhraseMatch m;
  REQUIRE(pb.match(toks, 0, m));
  CHECK(m.slots.size() == 2);
  CHECK(m.slots[0].kind == frame::SlotKind::Adj);
  CHECK(m.slots[1].kind == frame::SlotKind::NP);
  stdfs::remove(dir / "states_en_la.tsv", ec);
  Result<curated::CuratedData> r2 = curated::CuratedData::load(dir);
  REQUIRE(!r2.ok());
  CHECK(r2.error().hint.find("states_en_la.tsv") != std::string::npos);
  stdfs::remove_all(dir, ec);
}

TEST_CASE("rules-c: lexical selection prefers core words of the right sense (fidelity 2) and the teacher's glosses") {
  NEED_REAL();
  transfer::Transfer tr(real().la, cur());
  auto pick = [&](const char* w, uint8_t pos, int fid = 2) {
    transfer::Settings st;
    st.fidelity = fid;
    transfer::Choice ch;
    const uint32_t id = tr.select(w, pos, {}, false, false, st, ch);
    return id == lex::kNoLemma ? std::string("-") : text::latin_key(real().la.lemma(id).head);
  };
  CHECK(pick("song", feat::Noun) == "carmen");
  CHECK(pick("clock", feat::Noun) == "horologium");
  CHECK(pick("smile", feat::Verb) == "rideo");
  CHECK(pick("smile", feat::Verb, 1) == "subrideo");   // faithful mode keeps the exact word
  CHECK(pick("hole", feat::Noun) == "fouea");
  CHECK(pick("bottom", feat::Noun) == "imus");          // a taught adjective as a noun ("in īmō")
  CHECK(pick("letter", feat::Noun) == "epistula");
  CHECK(pick("dark", feat::Noun) == "tenebrae");
  CHECK(pick("shake", feat::Verb) == "tremo");
  CHECK(pick("strange", feat::Adj) == "mirus");
  CHECK(pick("child", feat::Noun) == "puer");
  CHECK(pick("help", feat::Verb) == "adiuuo");
  CHECK(pick("plant", feat::Verb) == "sero");
  CHECK(pick("paint", feat::Noun) == "color");
  CHECK(pick("coat", feat::Noun) == "pallium");
  CHECK(pick("rude", feat::Adj) == "inurbanus");
  CHECK(pick("tea", feat::Noun) == "thea");             // not speciēs (a weak sense of "tea")
  CHECK(pick("card", feat::Noun) == "charta");
}

namespace {
struct Out { std::string text; rules::Confidence conf; std::vector<std::string> flags; std::vector<rules::Check> checks; };
std::vector<Out> run(const std::vector<std::string>& src, char gender = 'f', int fidelity = 2) {
  static std::unique_ptr<rules::Engine> e = engine();
  std::vector<rules::CueInput> in;
  for (size_t i = 0; i < src.size(); ++i) {
    rules::CueInput c;
    c.index = (uint32_t)i;
    c.sourceText = src[i];
    c.startMs = (int64_t)i * 4000;
    c.endMs = c.startMs + 3500;
    in.push_back(c);
  }
  rules::Options o;
  o.speakerGender = gender;
  o.fidelity = fidelity;
  auto r = e->translate(in, o, rules::Context{}, nullptr, nullptr);
  REQUIRE(r.ok());
  std::vector<Out> out;
  for (const auto& c : r.value()) out.push_back(Out{flat(c.target), c.confidence, c.flags, c.checks});
  return out;
}
bool hasFlag(const Out& o, const char* f) { return std::find(o.flags.begin(), o.flags.end(), f) != o.flags.end(); }
}  // namespace

TEST_CASE("rules-c: constructions of quality loop 1 (one sentence each)") {
  NEED_REAL();
  const std::pair<const char*, const char*> cases[] = {
      {"Everyone bow!", "Omnēs, inclīnāte vōs!"},                      // phrasal_en_la.tsv bow - inclīnō refl
      {"Bow!", "Inclīnā tē!"},                                          // one-word imperative retagged
      {"Do you play cards?", "Lūdisne chartīs?"},                       // phrasebook vp row inside a clause
      {"Which way should I go?", "Quā viā īre dēbeō?"},                 // route ablative
      {"That depends on where you want to go.", "Id pendet ex eō quō īre vīs."},   // {WH} slot
      {"Then it doesn't matter which way you go.", "Nihil igitur interest quā viā eās."},   // {WH} + subjunctive
      {"You must be, or you wouldn't be here.", "Certē es; aliter hīc nōn essēs."},
      {"I thought it was Monday.", "Putābam diem Lūnae esse."},
      {"What day is it today?", "Quī diēs est hodiē?"},                 // order.wh.cop
      {"Can you help me with the letter?", "Potesne mē in epistulā adiuvāre?"},   // verbprep help with
      {"She is the Queen of Hearts.", "Rēgīna Cordium est."},           // 3rd-person pronoun dropped before a name
      {"Sit down and tell me a story.", "Sedē et nārrā mihi fābulam."}, // macron override in the Macrons primitive
      {"Have some tea.", "Sūme thēam."},
      {"I don't know any songs.", "Nūllum carmen sciō."},               // nūllus + singular
      {"Are you afraid of the dark?", "Timēsne tenebrās?"},             // states_en_la.tsv + plural-only noun
      {"I believe six impossible things before breakfast.", "Sex rēs quae fierī nōn possunt ante ientāculum crēdō."},
      {"The teacher is tired.", "Magister fessus est."},                // states_en_la.tsv kind adj
      {"She's very small and very white.", "Valdē parva et valdē alba est."},   // one copula, coordinated predicates
      {"Today the teacher was angry.", "Hodiē magister īrātus erat."},  // source starts with today
      {"The teacher was angry today.", "Magister hodiē īrātus erat."},  // ... and does not
      {"It's always six o'clock here.", "Semper hīc hōra sexta est."},
      {"Here the cat always sleeps.", "Hīc fēlēs semper dormit."},      // source order of the adverbs
      {"What a strange garden!", "Quam mīrus hortus!"},
      {"We live in a small house near the river.", "In domō parvā prope flūmen habitāmus."},
  };
  for (const auto& c : cases) {
    const std::vector<Out> o = run({c.first});
    CHECK_MESSAGE(o[0].text == c.second, c.first << " -> " << o[0].text << " (expected " << c.second << ")");
  }
  // "child" in address follows the main character's gender
  CHECK(run({"What is your name, child?"}, 'f')[0].text == "Quid est nōmen tibi, puella?");
  CHECK(run({"What is your name, child?"}, 'm')[0].text == "Quid est nōmen tibi, puer?");
  // an elliptical "where" takes the previous clause's motion
  const auto e = run({"That depends on where you want to go.", "I don't much care where."});
  CHECK(e[1].text == "Nōn multum cūrō quō.");
  CHECK(e[1].conf == rules::Confidence::Check);   // the tagger missed the verb: a fallback
  CHECK(hasFlag(e[1], "frame-fallback"));
  // fallbacks are Check: buried clause, retag
  CHECK(run({"I thought it was Monday."})[0].conf == rules::Confidence::Check);
  CHECK(run({"You must be, or you wouldn't be here."})[0].conf == rules::Confidence::Check);
}

TEST_CASE("rules-c: parser-failure helpers and the split retry") {
  CHECK(frame::FrameBuilder::splitPoints("You must be, or you wouldn't be here.") == std::vector<size_t>{13});
  CHECK(frame::FrameBuilder::splitPoints("Sit down; tell me a story, and then go.") == std::vector<size_t>{10, 27});
  CHECK(frame::FrameBuilder::splitPoints("Red, white and blue.").empty());
  NEED_REAL();
  frame::FrameBuilder fb(frame::SrcLang::En, &real().pen, &real().en, cur());
  frame::SemSentence s;
  fb.analyse("The cat could smile.", s);
  CHECK(!frame::FrameBuilder::troubled(s));
  CHECK(s.repairs.empty());
  fb.analyse("I thought it was Monday.", s);
  CHECK(std::find(s.repairs.begin(), s.repairs.end(), "clause-repair") != s.repairs.end());
  // a troubled sentence with a split point is translated in pieces, joined with ";", and marked Check
  fb.analyse("Well, the queen, and the cat sleeps.", s);
  const auto o = run({"Well, the queen, and the cat sleeps."});
  CHECK(!o[0].text.empty());
  if (frame::FrameBuilder::troubled(s)) {
    CHECK(o[0].conf != rules::Confidence::Ok);
    CHECK(hasFlag(o[0], "frame-fallback"));
  }
}

TEST_CASE("rules-c: \"you\" number from the reply and confidence of guesses and rare words") {
  NEED_REAL();
  const auto o = run({"Why are you painting the roses?", "We planted white roses by mistake."});
  CHECK(o[0].text == "Cūr rosās pingitis?");
  CHECK(o[0].conf == rules::Confidence::Check);
  CHECK(hasFlag(o[0], "addressee-guess"));
  CHECK(run({"Why are you painting the roses?"})[0].text == "Cūr rosās pingis?");   // no reply: singular
  // fidelity 1 keeps subrīdeō, but a core word of the same sense existed: Check
  const auto f1 = run({"The cat could smile."}, 'f', 1);
  CHECK(f1[0].text == "Fēlēs subrīdēre poterat.");
  CHECK(f1[0].conf == rules::Confidence::Check);
  CHECK(hasFlag(f1[0], "low-tier"));
  CHECK(run({"The cat could smile."}, 'f', 2)[0].conf == rules::Confidence::Ok);
}

TEST_CASE("rules-c: tag policy on CueInput.spans (A5 'tag position approximated' is Check, not Fix)") {
  NEED_REAL();
  auto e = engine();
  auto cueFrom = [](uint32_t idx, const std::string& raw, subs::Format fmt) {
    rules::CueInput c;
    c.index = idx;
    std::string plain;
    for (const subs::Span& sp : subs::splitSpans(raw, fmt)) {
      c.spans.push_back(rules::SpanIn{sp.kind == subs::Span::Tag, sp.raw});
      if (sp.kind == subs::Span::Text) plain += sp.raw;
    }
    c.sourceText = plain;
    c.startMs = idx * 4000;
    c.endMs = c.startMs + 3500;
    return c;
  };
  std::vector<rules::CueInput> in = {cueFrom(0, "<i>Where are you going?</i>", subs::Format::Srt),
                                     cueFrom(1, "<i>Open the door, please.</i>", subs::Format::Srt),
                                     cueFrom(2, "{\\an8}Who are you?", subs::Format::Ass),
                                     cueFrom(3, "I don't <i>know</i>.", subs::Format::Srt)};
  auto r = e->translate(in, rules::Options{}, rules::Context{}, nullptr, nullptr);
  REQUIRE(r.ok());
  auto a5 = [](const rules::CueOutput& c) {
    for (const auto& k : c.checks)
      if (k.id == "A5") return k;
    return rules::Check{};
  };
  for (int i = 0; i < 3; ++i) {
    CHECK_MESSAGE(a5(r.value()[(size_t)i]).ok, "cue " << i << ": " << a5(r.value()[(size_t)i]).detail);
    CHECK(std::find(r.value()[(size_t)i].flags.begin(), r.value()[(size_t)i].flags.end(), "tags") !=
          r.value()[(size_t)i].flags.end());
  }
  CHECK(r.value()[0].target == "Quō īs?");
  const rules::CueOutput& part = r.value()[3];
  CHECK(!a5(part).ok);
  CHECK(a5(part).detail == "tag position approximated");
  CHECK(part.confidence == rules::Confidence::Check);
  CHECK(std::find(part.flags.begin(), part.flags.end(), "tags-approximated") != part.flags.end());
}

TEST_CASE("rules-c: displayForm drops tie bars; Macrons applies overrides (also capitalised)") {
  CHECK(morph::displayForm("de\xCD\xA1inde", true) == "deinde");
  curated::MacronOverride ov{"narro", "narr", "nārr", ""};
  CHECK(realise::Macrons::apply("narrā", true, &ov) == "nārrā");
  CHECK(realise::Macrons::apply("Narrā", true, &ov) == "Nārrā");
  CHECK(realise::Macrons::apply("narrā", false, &ov) == "narra");
  CHECK(realise::Macrons::apply("amō", true, nullptr) == "amō");
}

TEST_CASE("rules-en: RSS flat over 1,026 cues (the regression file 9 times)") {
  NEED_REAL();
  auto e = engine();
  std::vector<rules::CueInput> all = regressionCues(9);
  REQUIRE(all.size() == 1026);
  rules::Options o;
  rules::Context ctx;
  std::vector<rules::CueInput> ten(all.begin(), all.begin() + 10);
  REQUIRE(e->translate(ten, o, ctx, nullptr, nullptr).ok());   // warm up: models, tables, realiser
  REQUIRE(e->translate(ten, o, ctx, nullptr, nullptr).ok());
  const long after10 = rssAnonKb();
  for (size_t at = 0; at < all.size(); at += 20) {   // CLI-sized batches
    std::vector<rules::CueInput> b(all.begin() + (long)at, all.begin() + (long)std::min(all.size(), at + 20));
    if (at) b.front().prevSource = all[at - 1].sourceText;
    REQUIRE(e->translate(b, o, ctx, nullptr, nullptr).ok());
  }
  const long after1000 = rssAnonKb();
  MESSAGE("RssAnon after 10 cues: " << after10 << " kB, after 1,026 cues: " << after1000 << " kB");
#if defined(__SANITIZE_ADDRESS__)
  MESSAGE("AddressSanitizer build: its quarantine keeps freed blocks resident; RSS bound checked in normal builds");
#else
  if (after10 > 0) CHECK(after1000 <= after10 + after10 / 20 + 256);   // 5 % (+256 kB allocator slack)
#endif
}

// ================================================================================================================
// C15 (quality loop 2): the burned public-domain sample tests/regression/oz_sample.en.srt vs the main agent's gold.

TEST_CASE("rules-d: end to end on oz_sample.en.srt vs the gold Latin (report; determinism; calibration)") {
  NEED_REAL();
  auto e = engine();
  std::vector<rules::CueInput> in = regressionCues(1, "oz_sample.en.srt");
  REQUIRE(in.size() == 100);
  rules::Options o;
  o.fidelity = 2;
  o.speakerGender = 'f';   // the gold file's speaker is usually Dorothy (header of the gold file)
  rules::Context ctx;
  auto r1 = e->translate(in, o, ctx, nullptr, nullptr);
  REQUIRE(r1.ok());
  auto r2 = engine()->translate(in, o, ctx, nullptr, nullptr);   // a fresh engine: byte-identical
  REQUIRE(r2.ok());
  REQUIRE(r1->size() == 100);
  REQUIRE(r2->size() == 100);
  bool same = true;
  for (size_t i = 0; i < 100; ++i) {
    same = same && r1.value()[i].target == r2.value()[i].target;
    same = same && r1.value()[i].confidence == r2.value()[i].confidence;
  }
  CHECK(same);
  std::ifstream g(repo() / "tests" / "regression" / "expected" / "oz_sample.la.gold.txt");
  std::vector<std::string> gold;
  std::string line;
  while (std::getline(g, line))
    if (!line.empty() && line[0] != '#') gold.push_back(line);
  REQUIRE(gold.size() == 100);
  // OK cues whose Latin is a correct variant the gold does not list yet: proposed gold alternatives (docs/rules_en_notes.md
  // "Quality loop 2"); every other wrong cue must not be OK
  const std::map<size_t, std::string> proposed = {{1, "Itaque adhūc sāgās et magōs inter nōs habēmus."},
                                                   {41, "Ō, grātiās tibi agō!"}, {82, "Cūr nōn anteā dē eō cōgitāvimus?"}};
  int matches = 0, exact = 0, wrongOk = 0, wrongOkProposed = 0;
  std::map<std::string, int> conf, checkWhy;
  std::ostringstream table;
  table << "| # | source | gold | ours | conf |\n|---|---|---|---|---|\n";
  for (size_t i = 0; i < 100; ++i) {
    const rules::CueOutput& c = r1.value()[i];
    CHECK_MESSAGE(!c.target.empty(), "empty target for cue " << i + 1);
    ++conf[confName(c.confidence)];
    const std::string ours = flat(c.target);
    bool match = false, exactMatch = false;
    for (const std::string& alt : splitAlt(gold[i])) {
      match = match || norm(alt) == norm(ours);
      exactMatch = exactMatch || text::nfc(alt) == text::nfc(ours);
    }
    matches += match;
    exact += exactMatch;
    if (!match && c.confidence == rules::Confidence::Ok) {
      auto pr = proposed.find(i + 1);
      if (pr != proposed.end() && norm(pr->second) == norm(ours)) ++wrongOkProposed;
      else ++wrongOk;
    }
    if (c.confidence != rules::Confidence::Ok) {
      bool why = false;
      for (const auto& f : c.flags)
        if (f != "tags") { ++checkWhy[f]; why = true; }
      for (const auto& k : c.checks)
        if (!k.ok) { ++checkWhy[k.id]; why = true; }
      if (!why) ++checkWhy["margin < 0.15"];
    }
    if (!match) {
      std::string chk;
      for (const auto& k : c.checks)
        if (!k.ok) chk += k.id + " ";
      for (const auto& f : c.flags)
        if (f != "tags") chk += f + " ";
      std::string g2 = gold[i];
      for (size_t at; (at = g2.find(" | ")) != std::string::npos;) g2.replace(at, 3, " / ");
      table << "| " << i + 1 << " | " << in[i].sourceText << " | " << g2 << " | " << ours << " | "
            << confName(c.confidence) << (chk.empty() ? "" : " " + chk) << "|\n";
    }
  }
  std::ostringstream rep;
  rep << "Regression oz_sample.en.srt -> Latin, fidelity 2, speaker f\n";
  rep << "match rate (normalised, any gold alternative): " << matches << " / 100\n";
  rep << "exact (macrons and punctuation too): " << exact << " / 100\n";
  rep << "confidence: ok " << conf["ok"] << ", check " << conf["check"] << ", fix " << conf["fix"] << "\n";
  rep << "wrong among OK: " << wrongOk << " (plus " << wrongOkProposed << " with a proposed gold alternative)\n";
  rep << "check/fix because:";
  for (const auto& w : checkWhy) rep << " " << w.first << " " << w.second << ";";
  rep << "\n\nMismatches:\n" << table.str() << "\nAll outputs:\n";
  for (size_t i = 0; i < 100; ++i)
    rep << i + 1 << "\t" << in[i].sourceText << "\t" << flat(r1.value()[i].target) << "\t"
        << confName(r1.value()[i].confidence) << "\n";
  std::ofstream(buildDir() / "regression_report_oz.txt") << rep.str();
  MESSAGE("oz regression: " << matches << " / 100 match the gold; confidence ok " << conf["ok"] << " / check "
                            << conf["check"] << " / fix " << conf["fix"] << "; wrong among OK " << wrongOk);
  CHECK(wrongOk == 0);
  CHECK(matches >= 84);   // C15 50 / 100; C17 68 / 100; C19 74 / 100; C26 84 / 100 without the gold alternatives it proposes (docs/rules_en_notes.md)
}

TEST_CASE("rules-d: tests/samples/sample.en.srt, 12 cues with the expected Latin") {
  NEED_REAL();
  std::ifstream f(repo() / "tests" / "samples" / "sample.en.srt", std::ios::binary);
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
  auto d = subs::parse(b, subs::Format::Srt);
  REQUIRE(d.ok());
  REQUIRE(d->cues.size() == 12);
  std::vector<std::string> src;
  for (const auto& c : d->cues) src.push_back(c.plainText());
  const char* expected[] = {"Puella rosam videt.",          "Nauta in īnsulā habitat.",  "Agricola aquam portat.",
                            "Marcus librum legit.",         "Canis in viā dormit.",      "Puerī in hortō lūdunt.",
                            "Māter fīliam suam vocat.",     "Sōl in caelō lūcet.",       "Lupus per silvam currit.",
                            "Discipulī magistrum audiunt.", "Nāvis ad portum venit.",    "Avis in arbore canit."};
  const auto o = run(src, 'f');
  REQUIRE(o.size() == 12);
  for (size_t i = 0; i < 12; ++i) {
    CHECK_MESSAGE(o[i].text == expected[i], src[i] << " -> " << o[i].text << " (expected " << expected[i] << ")");
    CHECK(o[i].conf != rules::Confidence::Fix);
  }
}

TEST_CASE("rules-d: never nonsense (a failed parse is a literal word list marked Fix)") {
  NEED_REAL();
  // the tagger reads "farmer" as a comparative and "carries" as a noun: the root cause is repaired (retag + clause)
  CHECK(run({"The farmer carries water."})[0].text == "Agricola aquam portat.");
  // no verb anywhere although a word can be one: no sentence is invented
  const auto g = run({"Colorless green ideas sleep furiously."});
  CHECK(g[0].conf == rules::Confidence::Fix);
  CHECK(hasFlag(g[0], "could-not-parse"));
  CHECK(!g[0].text.empty());
  CHECK(g[0].text.find("[Colorless]") != std::string::npos);   // unknown words stay in brackets
  // a clause cut from its sentence is never OK
  const auto fr = run({"and the bees cannot sting them."});
  CHECK(fr[0].text == "Et apēs eōs pungere nōn possunt.");
  CHECK(fr[0].conf != rules::Confidence::Ok);
  CHECK(hasFlag(fr[0], "fragment"));
}

TEST_CASE("rules-d: token hygiene (no brackets or punctuation inside words) and sense head words") {
  CHECK(morph::cleanHead("((caelum") == "caelum");
  CHECK(morph::cleanHead("alius))") == "alius");
  CHECK(morph::cleanHead("[verb]") == "[verb]");   // an unknown word keeps its marks
  CHECK(morph::displayForm("((caelum", true) == "caelum");
  NEED_REAL();
  for (const char* sent : {"The sun shines in the sky.", "There is no place like home.", "The other boys are here."}) {
    rules::CueInput c;
    c.sourceText = sent;
    c.startMs = 0;
    c.endMs = 4000;
    auto r = engine()->translate({c}, rules::Options{}, rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    for (const auto& t : r.value()[0].tokens) {
      CHECK_MESSAGE(t.text.find('(') == std::string::npos, sent << ": token " << t.text);
      CHECK_MESSAGE(t.text.find(')') == std::string::npos, sent << ": token " << t.text);
    }
  }
  transfer::Transfer tr(real().la, cur());
  auto pick = [&](const char* w, uint8_t pos) {
    transfer::Settings st;
    transfer::Choice ch;
    const uint32_t id = tr.select(w, pos, {}, false, false, st, ch);
    return id == lex::kNoLemma ? std::string("-") : text::latin_key(morph::cleanHead(real().la.lemma(id).head));
  };
  CHECK(pick("harbour", feat::Noun) == "portus");   // the gloss head beats "harbours" inside portōrium's gloss
  CHECK(pick("harbor", feat::Noun) == "portus");
  CHECK(pick("man", feat::Noun) == "uir");
  CHECK(pick("straw", feat::Noun) == "palea");
  CHECK(pick("comrade", feat::Noun) == "socius");
  CHECK(pick("other", feat::Adj) == "alius");
  CHECK(pick("bring", feat::Verb) == "fero");
  CHECK(pick("carry", feat::Verb) == "porto");
  CHECK(pick("seek", feat::Verb) == "quaero");
}

TEST_CASE("rules-d: cue mapping keeps clause fragments apart; names of the tuning sample") {
  auto s = frame::mapSentences({"I am anxious to get back to my aunt and uncle,", "Can you help me find my way?",
                                "But to those who are not honest,", "and if you wish for anything ring the bell.",
                                "I was walking", "to the river."});
  REQUIRE(s.size() == 5);
  CHECK(s[0].text == "I am anxious to get back to my aunt and uncle,");
  CHECK(s[2].text == "But to those who are not honest,");
  CHECK(s[4].text == "I was walking to the river.");
  NEED_REAL();
  const std::pair<const char*, const char*> cases[] = {
      {"Then you must go to the City of Emeralds.", "Tum ad Urbem Smaragdōrum īre dēbēs."},
      {"The Scarecrow saved the Tin Woodman.", "Terriculum Lignātōrem Stanneum servāvit."},
      {"Even if I wanted to, how could I kill the Wicked Witch?", "Etiam sī vellem, quōmodo Sāgam Malam necāre possem?"},
      {"Dorothy loves Kansas.", "Dorothēa Kansiam amat."},
      {"And back to Kansas?", "Et in Kansiam?"},
      {"Perhaps Oz will help you.", "Fortasse Oz tē adiuvābit."},
      {"the Great Wizard I told you of.", "Magnus Magus dē quō tibi dīxī."},
      {"I am a Cowardly Lion.", "Leō Timidus sum."},
  };
  for (const auto& c : cases) {
    if (!*c.second) continue;
    CHECK_MESSAGE(run({c.first})[0].text == c.second, c.first << " -> " << run({c.first})[0].text);
  }
}

TEST_CASE("rules-d: constructions of quality loop 2 (one sentence each)") {
  NEED_REAL();
  const std::pair<const char*, const char*> cases[] = {
      // fragments and discourse
      {"But, comrades, what shall we do now?", "Sed, sociī, quid nunc faciēmus?"},
      {"Why, don't you know?", "Quid? Nescīs?"},
      {"Can't you get down?", "Nōn potes dēscendere?"},
      {"My head is full, you know,", "Caput meum plēnum est, ut scīs,"},
      {"I cannot do it, however; you must try.", "Tamen id facere nōn possum; cōnārī dēbēs."},
      {"Oh, your Majesty, we are here!", "Ō, Māiestās Tua, hīc sumus!"},
      {"Sit down, my dear.", "Sedē, mea cāra."},
      {"For you will help me.", "Nam mē adiuvābis."},
      {"And then we can go.", "Et tum īre possumus."},
      {"Exactly so!", "Ita plānē!"},
      // comparison
      {"He is more powerful than his brother.", "Potentior est quam frāter suus."},
      {"That is greater than the sea.", "Id maius est quam mare."},
      {"She is as tall as her mother.", "Tam alta est quam māter sua."},
      {"They are so tired that they cannot walk.", "Tam fessī sunt ut ambulāre nōn possint."},
      // purpose, complements, modals
      {"I came to see you.", "Vēnī ut tē videam."},
      {"I want to see you.", "Tē vidēre volō."},
      {"Can you help me find my way?", "Potesne mē adiuvāre ut viam inveniam?"},   // C17: phrasebook vp "find my way" (oz gold #4)
      {"I ordered them to build the house.", "Eōs iussī domum aedificāre."},
      {"I ordered them to build this City and my Palace.", "Eōs iussī hanc Urbem et Rēgiam meam aedificāre."},
      {"for you will help to keep away the other wild beasts.", "Nam aliās bēstiās ferās arcēre adiuvābis."},
      {"I could not move.", "Movēre nōn poteram."},
      {"We have to go.", "Īre dēbēmus."},
      {"Shall we go there?", "Eāmusne illūc?"},
      {"I should like to cry.", "Flēre velim."},
      {"I always like to help my friends.", "Semper amīcōs meōs libenter adiuvō."},
      // passives and reported speech
      {"The boy was kissed by the girl.", "Puella puerum ōsculāta est."},   // deponent: said actively
      {"The letter was written by the queen.", "Epistula ā rēgīnā scrīpta est."},
      {"It is said that he is wise.", "Dīcitur prūdēns esse."},
      {"We have been told that the queen is kind.", "Nōbīs dictum est rēgīnam benignam esse."},
      {"She said that the boy was tired.", "Dīxit puerum fessum esse."},
      {"Some say he looks like a cat.", "Quīdam dīcunt eum fēlī similem esse."},
      // indefinites and negation
      {"No one knows.", "Nēmō scit."},
      {"I do not want to kill anybody.", "Nēminem necāre volō."},
      {"and if you wish for anything ring the bell.", "Et sī quid vīs, tintinnābulum pulsā."},
      {"I have no heart.", "Cor nōn habeō."},
      // clauses
      {"The woman who sings is my mother.", "Fēmina quae canit māter mea est."},
      {"The book that I read is long.", "Liber quem lēgī longus est."},
      {"When the teacher came, we were sleeping.", "Cum magister vēnit, dormiēbāmus."},
      {"I will wait until you come.", "Manēbō dum venīs."},
      {"Unless you go, I will stay.", "Nisi īs, manēbō."},
      {"I don't know whether he is here.", "Nesciō num hīc sit."},
      {"See what you have done!", "Vidē quid fēcerīs!"},
      {"Where did you get the shoes?", "Unde calceōs accēpistī?"},
      // tense and lexical
      {"Oz is gone.", "Oz abiit."},
      {"Oz was always our friend.", "Oz semper amīcus noster fuit."},
      {"Oz will send for you tomorrow morning.", "Oz crās māne tē arcesset."},
      {"I am all tired out.", "Omnīnō fessa sum."},
      {"how am I to get back to Kansas?", "Quōmodo in Kansiam redībō?"},
  };
  for (const auto& c : cases) {
    const std::vector<Out> o = run({c.first});
    CHECK_MESSAGE(o[0].text == c.second, c.first << " -> " << o[0].text << " (expected " << c.second << ")");
  }
}


// ================================================================================================================
// C17 (quality loop 3). Every rule is tested on sentences of our own, written before the tuning sample was run; the
// oz_sample report is never the only evidence (docs/rules_en_notes.md "Quality loop 3").

TEST_CASE("rules-e: base words of unknown English forms (spelling only)") {
  using frame::en::baseCandidates;
  auto has = [](const std::vector<std::string>& v, const char* w) { return std::find(v.begin(), v.end(), w) != v.end(); };
  CHECK(has(baseCandidates("stopped", "VERB"), "stop"));     // doubled consonant
  CHECK(has(baseCandidates("hoped", "VERB"), "hope"));       // dropped e
  CHECK(has(baseCandidates("carried", "VERB"), "carry"));    // y -> ied
  CHECK(has(baseCandidates("babies", "NOUN"), "baby"));      // y -> ies
  CHECK(has(baseCandidates("bigger", "ADJ"), "big"));
  CHECK(has(baseCandidates("braver", "ADJ"), "brave"));
  CHECK(has(baseCandidates("happiest", "ADJ"), "happy"));
  CHECK(has(baseCandidates("gently", "ADV"), "gentle"));
  CHECK(has(baseCandidates("sadly", "ADV"), "sad"));
  CHECK(baseCandidates("bran-new")[1] == "new");             // hyphenated: joined, then the last part
  CHECK(baseCandidates("sea-shore")[0] == "seashore");
  CHECK(!has(baseCandidates("kindness", "NOUN"), "kind"));   // no -ness: a noun is not its adjective
  CHECK(!has(baseCandidates("glass", "NOUN"), "glas"));
  NEED_REAL();
  CHECK(frame::en::verbOfForm(real().en, "sang") == "sing");
  CHECK(frame::en::verbOfForm(real().en, "lighted") == "light");
  bool present = false;
  CHECK(frame::en::verbOfForm(real().en, "sleeping", &present) == "sleep");
  CHECK(present);
}

TEST_CASE("rules-e: unknown and misread English forms are derived from known lemmas (work item a)") {
  NEED_REAL();
  const std::pair<const char*, const char*> cases[] = {
      // a participle used as an adjective: the Latin verb's participle
      {"She carried a lighted candle.", "Candēlam accēnsam portāvit."},
      {"The lighted candle stood on the table.", "Candēla accēnsa in mēnsā stābat."},
      {"The boat was broken.", ""},
      // a comparative the tagger read as a noun
      {"The boy is braver than his sister.", "Puer fortior est quam soror sua."},
      {"My sister is braver than my brother.", "Soror mea fortior est quam frāter meus."},
      // an irregular past the tagger read as a present, or as a noun
      {"The girl sang a song.", "Puella carmen cecinit."},
      {"He swam across the river.", "Trāns flūmen nāvit."},
      // hyphenated compounds: joined, else the head
      {"We have a brand-new house.", "Domum novam habēmus."},
      {"We wanted a brand-new coat.", "Pallium novum voluimus."},
      {"The sea-shore was quiet.", "Lītus tranquillum erat."},
      // a verb hung under a noun: the flat clause is rebuilt
      {"The tired horses drank water.", "Equī fessī aquam bibērunt."},
      {"The farmer's son carried water.", "Fīlius agricolae aquam portāvit."},
      // an adjective tagged as an adverb between a determiner and a noun
      {"You are a clever girl.", "Puella callida es."},
      // plural nouns read as verbs before their own verb
      {"Only witches wear black hats.", "Sōlae sāgae pilleōs nigrōs gerunt."},   // C19: hat -> pilleus (tier row),
      // English plural-only nouns
      {"She washed the clothes.", "Vestēs lāvit."},
  };
  for (const auto& c : cases) {
    if (!*c.second) continue;
    const std::vector<Out> o = run({c.first});
    CHECK_MESSAGE(o[0].text == c.second, c.first << " -> " << o[0].text << " (expected " << c.second << ")");
    CHECK_MESSAGE(o[0].text.find('[') == std::string::npos, c.first << " -> " << o[0].text);
  }
}

TEST_CASE("rules-e: participles and complements (work items d, e)") {
  NEED_REAL();
  const std::pair<const char*, const char*> cases[] = {
      // object complements of factitive verbs
      {"They made him king.", "Eum rēgem fēcērunt."},
      {"The queen made the girl her friend.", "Rēgīna puellam amīcam suam fēcit."},
      {"The rain made the road wet.", "Pluvia viam ūmidam fēcit."},
      // participle / adjective phrases after a comma
      {"The box was old, covered with dust.", "Arca vetus erat, pulvere tēcta."},
      {"She is a brave girl, afraid of nothing.", "Puella fortis est, nihil timēns."},
      // participle fragments (a clause cut from its sentence)
      {"and the little boat broken by the waves.", "Et nāvis parva undīs frācta."},
      {"and the queen loved by everyone.", "Et rēgīna ab omnibus amāta."},
      {"and the letter written by the queen.", "Et epistula ā rēgīnā scrīpta."},
      {"or the old cart badly broken on the stones below.", "Aut plaustrum vetus in saxīs īnfrā graviter frāctum."},
      // "would" in reported speech: future infinitive agreeing with its subject
      {"He said he would come tomorrow.", "Dīxit sē crās ventūrum esse."},
      {"The girls said they would come.", "Puellae dīxērunt sē ventūrās esse."},
      {"The queen said she would help us.", "Rēgīna dīxit sē nōs adiūtūram esse."},
      // wh + to-infinitive: indirect question in the subjunctive, person of the one told
      {"Show me how to make bread.", "Mōnstrā mihi quōmodo pānem faciam."},
      {"Tell him where to go.", "Dīc eī quō eat."},
      {"He asked me what to do.", "Mē rogāvit quid facerem."},
      {"She taught us how to swim.", "Nōs docuit nāre."},
      // what ... looks like -> quālis
      {"What did the house look like?", "Quālis erat domus?"},
      {"I don't know what he looks like.", "Nesciō quālis sit."},
      // so that -> ut + subjunctive; "as" after the main clause -> ut + indicative
      {"We ran so that we could see the king.", "Cucurrimus ut rēgem vidēre possēmus."},
      {"so that the children could sleep.", "Ut puerī dormīre possent."},
      {"I will help you, as I promised.", "Tē adiuvābō ut prōmīsī."},
      // become + predicate
      {"The frog became a prince.", "Rāna princeps facta est."},
      // relative "where"
      {"The house where I live is small.", "Domus in quā habitō parva est."},
      // as + noun (role)
      {"He worked as a cook.", "Ut coquus labōrāvit."},
      {"She worked as a cook.", "Ut coqua labōrāvit."},
  };
  for (const auto& c : cases) {
    const std::vector<Out> o = run({c.first});
    CHECK_MESSAGE(o[0].text == c.second, c.first << " -> " << o[0].text << " (expected " << c.second << ")");
  }
  CHECK(hasFlag(run({"and the queen loved by everyone."})[0], "participle-phrase"));
}

TEST_CASE("rules-e: idioms, light verbs, quantities, places and order (work item c)") {
  NEED_REAL();
  const std::pair<const char*, const char*> cases[] = {
      {"Let's take a walk.", "Ambulēmus."},
      {"You made a mistake.", "Errāvistī."},                     // a light verb is an event (perfect)
      {"We had a rest under the tree.", "Sub arbore quiēvimus."},
      {"The cruel king starved the prisoners.", "Rēx saevus captīvōs famē cōnfēcit."},
      {"We have no right to take the apples.", "Nōn licet nōbīs māla sūmere."},
      {"It is no trouble to carry the box.", "Nūllus labor est arcam portāre."},
      {"We could not find our way in the forest.", "Viam in silvā invenīre nōn poterāmus."},
      {"I feel like a new man.", "Novus homō mihi videor."},
      {"They took the ball from me.", "Pilam mihi abstulērunt."},  // take from a person: dative
      {"He took the apple from the table.", "Mālum ā mēnsā abstulit."},
      {"He admitted me to his presence.", "Mē ad sē admīsit."},
      {"Do not speak in my presence.", "Cōram mē nōlī loquī."},
      {"I want to go home.", "Domum īre volō."},
      {"She stayed at home.", "Domī mānsit."},
      {"We have lots of apples.", "Multa māla habēmus."},
      {"There is a lot of water in the well.", "In puteō est multum aquae."},
      {"I saw the sea for the first time.", "Prīmum mare vīdī."},
      {"I saw a big and ugly dog.", "Canem magnum et foedum vīdī."},
      {"She laughed a little.", "Paulum rīsit."},
      {"We rested a little and then we walked on.", "Paulum requiēvimus et tum perrēximus."},
      {"I will of course help you.", "Certē tē adiuvābō."},
      {"She was in fact very kind.", "Rē vērā valdē benigna erat."},
      {"We found the house at last.", "Domum tandem invēnimus."},
      {"He sang so badly that we laughed.", "Tam male cecinit ut rīserīmus."},
      {"She walked very slowly.", "Valdē lentē ambulāvit."},
      {"How was it that you found me?", "Quōmodo mē invēnistī?"},
      {"Why is it that the sky is blue?", "Cūr caelum caeruleum est?"},
      {"Men are often foolish.", "Hominēs saepe stultī sunt."},
      {"I like red.", "Rubrum amō."},
      // agreement: two-gender nouns, persons
      {"The sky is blue.", "Caelum caeruleum est."},
      {"She is only a child.", "Puella tantum est."},
      {"My sister is a teacher.", "Soror mea magistra est."},
      {"The boy and the girl were tired.", "Puer et puella fessī erant."},
      // epithets and exclamations
      {"Then the brave Queen opened the gate.", "Tum fortis Rēgīna portam aperuit."},
      {"I saw the little Dorothy.", "Parvam Dorothēam vīdī."},
      {"You are a clever girl!", "Callida puella es!"},
      {"You are a wicked queen!", "Mala rēgīna es!"},
  };
  for (const auto& c : cases) {
    const std::vector<Out> o = run({c.first});
    CHECK_MESSAGE(o[0].text == c.second, c.first << " -> " << o[0].text << " (expected " << c.second << ")");
  }
  // A3 accepts the agreement the realiser makes (no Fix on these)
  for (const char* s : {"The boy and the girl were tired.", "The queen said she would help us.", "I feel like a new man.",
                        "There is a lot of water in the well.", "The house where I live is small.", "I know how to swim."})
    CHECK_MESSAGE(run({s})[0].conf != rules::Confidence::Fix, s);
}

TEST_CASE("rules-e: names, brackets, fragments (work items f, g, h)") {
  NEED_REAL();
  // a capitalised unknown word before a verb is a name: kept as written, Check (never Fix)
  for (const std::pair<const char*, const char*>& c :
       {std::pair<const char*, const char*>{"Grimbly ate the cake.", "Grimbly placentam ēdit."},
        {"Old Grumbo laughed.", "Vetus Grumbo rīsit."},
        {"We saw Flimsy and Grub.", "Flimsy et Grub vīdimus."},
        {"The Scarecrow and the Lion were happy.", "Terriculum et Leō laetī erant."}}) {
    const Out o = run({c.first})[0];
    CHECK_MESSAGE(o.text == c.second, c.first << " -> " << o.text);
    CHECK(o.conf != rules::Confidence::Fix);
  }
  // editorial text in square brackets inside a sentence: translated, brackets kept; a sound after a finished sentence
  // stays a nonverbal piece
  const auto ed = frame::mapSentences({"They are rusted [so badly] that I cannot move them.", "Wait! [door opens] Come in."});
  REQUIRE(ed.size() == 4);
  CHECK(ed[0].text == "They are rusted [so badly] that I cannot move them.");
  CHECK(ed[2].kind == frame::CueKind::Nonverbal);
  const Out b = run({"They are rusted [so badly] that I cannot move them at all;"})[0];
  CHECK(b.text == "[Tam male] rōbīginātī sunt ut eōs omnīnō movēre nōn possim;");
  CHECK(hasFlag(b, "editorial"));
  const Out b2 = run({"The queen was [very, very] angry."})[0];
  CHECK(b2.conf != rules::Confidence::Ok);
  // a fragment is still grammatical Latin and never OK
  const Out f = run({"so that the children could sleep."})[0];
  CHECK(f.text == "Ut puerī dormīre possent.");
  CHECK(hasFlag(f, "fragment"));
}

TEST_CASE("rules-e: noun consistency, same-headword lemmas (work item b; coordinator add-on)") {
  NEED_REAL();
  transfer::Transfer tr(real().la, cur());
  // two lemmas with the same cleaned head, part of speech and principal parts (genitive / infinitive / perfect) are one
  // candidate: they never compete with each other for the margin
  transfer::Settings st;
  transfer::Choice ch;
  const uint32_t sky = tr.select("sky", feat::Noun, {}, false, false, st, ch);
  REQUIRE(sky != lex::kNoLemma);
  std::vector<std::string> sigs;
  for (const auto& k : ch.candidates) {
    const lex::Lemma l = real().la.lemma(k.lemma);
    const morph::Principal pp = morph::parsePrincipal(morph::cleanHead(l.head), l.principal);
    sigs.push_back(text::nfc(morph::cleanHead(l.head)) + "|" + std::to_string((int)l.pos) + "|" + pp.infinitive + "|" +
                   pp.genitive + "|" + pp.perfect);
  }
  for (size_t i = 0; i < sigs.size(); ++i)
    for (size_t j = i + 1; j < sigs.size(); ++j) CHECK_MESSAGE(sigs[i] != sigs[j], sigs[i]);
  // homographs that inflect differently stay apart: volō "want" is not volō "fly"
  CHECK(run({"I want water."})[0].text == "Aquam volō.");
  // the Spanish regression cue that the duplicate "caelum" had made Check is OK again
  if (real().esOk) {
    auto e = engine();
    rules::CueInput c;
    c.sourceText = "¡Mira el cielo!";
    c.startMs = 0;
    c.endMs = 3000;
    rules::Options o;
    o.source = rules::Lang::Es;
    o.speakerGender = 'f';
    auto r = e->translate({c}, o, rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    CHECK(flat(r.value()[0].target) == "Spectā caelum!");
    CHECK(r.value()[0].confidence == rules::Confidence::Ok);
  }
  // consistency: a noun keeps the Latin word it had earlier in the batch when that word is a candidate here too
  frame::FrameBuilder fb(frame::SrcLang::En, &real().pen, &real().en, cur());
  frame::SemSentence s;
  fb.analyse("The witch saw the sorceress.", s);
  REQUIRE(!s.units.empty());
  transfer::Choice probe;
  tr.select("sorceress", feat::Noun, {}, false, false, st, probe);
  REQUIRE(probe.candidates.size() > 1);
  const uint32_t second = probe.candidates[1].lemma;
  for (uint32_t want : {probe.candidates[0].lemma, second}) {
    transfer::Memory mem;
    mem.nounSense.emplace_back("sorceress", want);
    transfer::ClauseOut co;
    tr.clause(s.units[0].frame, s, st, mem, co);
    CHECK(co.clause.hasObject);
    CHECK(co.clause.object.head == want);
  }
}

TEST_CASE("rules-e: fixes after the blind check (own sentences of children's dialogue)") {
  NEED_REAL();
  const std::pair<const char*, const char*> cases[] = {
      {"I am going to the market to buy some apples.", "Ad forum eō ut māla emam."},   // a goal: motion, not future
      {"She went to the river to wash the clothes.", "Ad flūmen iit ut vestēs lavet."},
      {"and nobody could help me look for it.", "Et nēmō mē adiuvāre poterat ut id quaererem."},   // sequence
      {"Please don't tell my brother about the secret door.", "Quaesō frātrī meō dē iānuā arcānā nōlī dīcere."},
      {"Tell the queen the truth.", "Dīc rēgīnae vēritātem."},
      {"I was tired, but I could not sleep.", "Fessa eram sed dormīre nōn poteram."},
      {"The rabbit hopped quickly into its hole.", "Cunīculus in foveam suam celeriter saluit."},
      {"The bird built its nest.", "Avis nīdum suum aedificāvit."},
      {"Hush, the baby is sleeping.", "Tacē, īnfāns dormit."},
      {"The fox was too clever for the hungry wolf.", "Vulpēs lupō ieiūnō nimis callida erat."},
      {"The children laughed when the clown fell down.", "Puerī rīsērunt cum scurra cecidit."},
      {"The old dog barked at the stranger all night.", "Canis vetus advenam tōtam noctem lātrāvit."},
      {"The babies cried all night.", "Īnfantēs tōtam noctem flēvērunt."},
      {"All the children laughed.", "Omnēs puerī rīsērunt."},
      {"Every morning I walk to school.", "Omnī māne ad lūdum ambulō."},
  };
  for (const auto& c : cases) {
    const std::vector<Out> o = run({c.first});
    CHECK_MESSAGE(o[0].text == c.second, c.first << " -> " << o[0].text << " (expected " << c.second << ")");
  }
  CHECK(run({"The children laughed when the clown fell down."})[0].conf != rules::Confidence::Fix);   // cum + nominative
}

// ================================================================================================================
// C19 (RULES-F): debugging hook. VP_RULES_TRY=<file>: every line is one cue (a line "---" starts a new batch, so
// independent sentences do not share the discourse memory when separated); prints target, confidence, flags, failed
// checks and the emoji tokens. VP_RULES_TRY_SPEAKER=m|f|u (default f), VP_RULES_TRY_FID=1|2|3 (default 2).
TEST_CASE("rules-f: try (VP_RULES_TRY=<file>)") {
  const char* env = std::getenv("VP_RULES_TRY");
  if (!env || !*env) return;
  NEED_REAL();
  const char* sg = std::getenv("VP_RULES_TRY_SPEAKER");
  const char* fd = std::getenv("VP_RULES_TRY_FID");
  std::ifstream f(env);
  std::vector<std::vector<std::string>> batches(1);
  std::string line;
  while (std::getline(f, line)) {
    if (line == "---") { batches.emplace_back(); continue; }
    if (!line.empty()) batches.back().push_back(line);
  }
  auto e = engine();
  int n = 0;
  for (const auto& b : batches) {
    if (b.empty()) continue;
    std::vector<rules::CueInput> in;
    for (size_t i = 0; i < b.size(); ++i) {
      rules::CueInput c;
      c.index = (uint32_t)i;
      c.sourceText = b[i];
      c.startMs = (int64_t)i * 4000;
      c.endMs = c.startMs + 3500;
      in.push_back(c);
    }
    rules::Options o;
    o.speakerGender = sg && *sg ? sg[0] : 'f';
    o.fidelity = fd && *fd ? std::atoi(fd) : 2;
    auto r = e->translate(in, o, rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    for (size_t i = 0; i < r->size(); ++i) {
      const rules::CueOutput& c = r.value()[i];
      std::string extra;
      for (const auto& fl : c.flags) extra += " " + fl;
      for (const auto& k : c.checks)
        if (!k.ok) extra += " " + k.id + "(" + k.detail + ")";
      std::string em;
      for (const auto& t : c.tokens)
        if (!t.emoji.empty()) em += " " + t.text + t.emoji;
      std::printf("%d\t%s\t%s\t%s |%s |%s\n", ++n, b[i].c_str(), flat(c.target).c_str(), confName(c.confidence),
                  extra.c_str(), em.c_str());
      if (std::getenv("VP_RULES_TRY_FRAME")) {
        static frame::FrameBuilder fb(frame::SrcLang::En, &real().pen, &real().en, cur());
        frame::SemSentence s;
        fb.analyse(b[i], s);
        std::string tk;
        for (const auto& t : s.tokens)
          tk += " " + t.text + "/" + t.upos + "/" + t.deprel + ">" + std::to_string(t.head) + "(" + fb.lemmaOf(t) + ")";
        std::printf("    tokens:%s\n    frame: %s\n", tk.c_str(), frame::describe(s).c_str());
      }
      if (std::getenv("VP_RULES_TRY_WHY"))
        for (const auto& rs : c.reasons)
          std::printf("    %s: %s %s\n", rs.kind.c_str(), rs.text.c_str(), rs.data.substr(0, 600).c_str());
    }
  }
}

// ================================================================================================================
// C19 (RULES-F, quality loop 4). Generalisation guard: every rule below has at least two sentences of our own, written
// before the rule was run on the tuning sample (docs/rules_en_notes.md "Quality loop 4").
namespace {
std::vector<Out> runBatch(const std::vector<std::string>& src, char gender = 'f') {
  std::unique_ptr<rules::Engine> e = engine();
  std::vector<rules::CueInput> in;
  for (size_t i = 0; i < src.size(); ++i) {
    rules::CueInput c;
    c.index = (uint32_t)i;
    c.sourceText = src[i];
    c.startMs = (int64_t)i * 4000;
    c.endMs = c.startMs + 3500;
    in.push_back(c);
  }
  rules::Options o;
  o.speakerGender = gender;
  auto r = e->translate(in, o, rules::Context{}, nullptr, nullptr);
  REQUIRE(r.ok());
  std::vector<Out> out;
  for (const auto& c : r.value()) out.push_back(Out{flat(c.target), c.confidence, c.flags, c.checks});
  return out;
}
void expectEach(const std::vector<std::pair<const char*, const char*>>& cases) {
  for (const auto& c : cases) {
    const std::vector<Out> o = run({c.first});
    CHECK_MESSAGE(o[0].text == c.second, c.first << " -> " << o[0].text << " (expected " << c.second << ")");
  }
}
}  // namespace

TEST_CASE("rules-f: fragments keep the case of their phrase (work item d)") {
  NEED_REAL();
  // a cue that is only a prepositional phrase: the preposition's case (the tagger read the lower-case preposition
  // or conjunction as a verb: retagged), never an invented verb or an unknown "[with]"
  expectEach({
      {"under the big table,", "Sub mēnsā magnā,"},
      {"into the dark forest,", "In silvam obscūram,"},
      {"to the little house.", "Ad domum parvam."},
      {"with a loud cry.", "Clāmōre magnō."},
      {"without a word.", "Sine verbō."},
      // a noun phrase alone: nominative; the possessive inside it is eius (no clause subject to refer to)
      {"and the queen's golden crown,", "Et corōna aurea rēgīnae,"},
      {"the old king and his three sons,", "Rēx vetus et trēs fīliī eius,"},
      // a possessive inside the subject itself is not reflexive either
      {"The king and his sons are here.", "Rēx et fīliī eius hīc sunt."},
  });
  for (const char* s : {"from the top of the hill,", "with her grandmother.", "for my mother.", "in the land of the giants,"}) {
    const std::vector<Out> o = run({s});
    CHECK_MESSAGE(o[0].conf != rules::Confidence::Fix, s << " -> " << o[0].text);
    CHECK_MESSAGE(o[0].text.find('[') == std::string::npos, s << " -> " << o[0].text);
  }
  // continuation of the previous cue (it ends open, with a comma): the conjunct takes the case of the last noun there
  {
    const std::vector<Out> o = runBatch({"I saw the king,", "and the queen."});
    CHECK_MESSAGE(o[1].text == "Et rēgīnam.", o[1].text);
    const std::vector<Out> p = runBatch({"We helped the farmer,", "and his wife."});
    CHECK_MESSAGE(p[1].text == "Et uxōrem eius.", p[1].text);
  }
  // a noun phrase with a prepositional modifier is a fragment, not a failed parse
  {
    const std::vector<Out> o = run({"a very small mouse with a long tail."});
    CHECK_MESSAGE(!hasFlag(o[0], "could-not-parse"), o[0].text);
    const std::vector<Out> p = run({"an old man with a white beard."});
    CHECK_MESSAGE(!hasFlag(p[0], "could-not-parse"), p[0].text);
  }
}

TEST_CASE("rules-f: unknown English words derived before they are given up (work item c)") {
  NEED_REAL();
  // compounds of a known word and a head noun (-man, -woman, -maid, -smith ...): head noun + genitive of the first
  // part; never a bracketed unknown (Check, the derivation is a guess)
  {
    const std::vector<std::pair<std::string, std::string>> parts = {
        {"snowman", "snow man"}, {"snowmen", "snow man"}, {"tinsmith", "tin smith"}, {"milkmaid", "milk maid"},
        {"doorkeeper", "door keeper"}, {"sandcastle", "sand castle"}};
    for (const auto& p : parts) {
      std::string first, head;
      CHECK_MESSAGE(frame::en::compoundParts(real().en, p.first, first, head), p.first);
      CHECK_MESSAGE(first + " " + head == p.second, p.first << " -> " << first << " " << head);
    }
    std::string f2, h2;
    CHECK(!frame::en::compoundParts(real().en, "kitten", f2, h2));   // not "kit" + "ten"
    CHECK(!frame::en::compoundParts(real().en, "table", f2, h2));
  }
  for (const char* s : {"The snowman is very big.", "The sandcastle fell down.", "The milkmaid sang a song.",
                        "Two snowmen stood in the garden."}) {
    const std::vector<Out> o = run({s});
    CHECK_MESSAGE(o[0].text.find('[') == std::string::npos, s << " -> " << o[0].text);
    CHECK_MESSAGE(o[0].conf != rules::Confidence::Fix, s << " -> " << o[0].text);
  }
  CHECK(run({"The snowman is very big."})[0].text.find("nivis") != std::string::npos);
  CHECK(run({"The milkmaid sang a song."})[0].text.find("lactis") != std::string::npos);
  // numerals written in words: every one is rendered (a dropped numeral was wrong and OK)
  expectEach({
      {"I have forty sheep.", "Quadrāgintā ovēs habeō."},
      {"Fifteen birds sang.", "Quīndecim avēs cecinērunt."},
      {"He has twenty-two cows.", "Vīgintī duōs bovēs habet."},
      {"We saw sixty ships.", "Sexāgintā nāvēs vīdimus."},
  });
  for (const char* s : {"He has twenty-one sheep.", "The ninety sheep ate grass.", "I counted thirty-one stars."}) {
    const std::vector<Out> o = run({s});
    const bool numeral = o[0].text.find("XXI") != std::string::npos || o[0].text.find("XXXI") != std::string::npos ||
                         o[0].text.find("nōnāgintā") != std::string::npos || o[0].text.find("Nōnāgintā") != std::string::npos;
    CHECK_MESSAGE(numeral, s << " -> " << o[0].text);
  }
  // contractions: 'd + past participle / better = had; 's + got / been = has; have got = have (possession)
  expectEach({
      {"He'd seen the wolf before.", "Anteā lupum vīderat."},
      {"She'd never been to the sea.", "Numquam ad mare fuerat."},
      {"I've got a new hat.", "Pilleum novum habeō."},
      {"He's got a big dog.", "Canem magnum habet."},
      {"We've got three cats.", "Trēs fēlēs habēmus."},
      {"You'd better go home.", "Domum īre dēbēs."},
      {"You'd better run.", "Currere dēbēs."},
  });
  // contraction rows (contractions_en.tsv)
  expectEach({
      {"Y'know, I'm hungry.", "Ut scīs, ēsuriō."},
      {"I'm tired, y'know.", "Fessa sum, ut scīs."},
      {"'Twas a cold night.", "Nox frīgida erat."},
      {"'Tis a cold night.", "Nox frīgida est."},
      {"Gimme the ball!", "Dā mihi pilam!"},
      {"Gimme some bread.", "Dā mihi pānem."},
      {"Lemme see.", "Sine mē vidēre."},
      {"Lemme help you.", "Sine mē tē adiuvāre."},
  });
  // interjections
  expectEach({
      {"Hurrah, we won!", "Iō, vīcimus!"},
      {"Hurrah! The snow has come.", "Iō! Nix vēnit."},
      {"Hooray, the snow!", "Iō, nix!"},
      {"Bravo, you did it!", "Euge, id fēcistī!"},
      {"Bravo, little brother!", "Euge, frāter parve!"},
  });
  // proper adjectives are adjectives, never dropped
  expectEach({
      {"He is a Roman soldier.", "Mīles Rōmānus est."},
      {"The Greek ship sailed away.", "Nāvis Graeca procul nāvigāvit."},
      {"I like Roman roads.", "Viae Rōmānae mihi placent."},
  });
}

TEST_CASE("rules-f: tagger and parser slips found on own sentences (C19)") {
  NEED_REAL();
  expectEach({
      // a copula with a place: "be" + prepositional phrase, never "Hortus est" ("it is a garden")
      {"She is in the garden.", "In hortō est."},
      {"The cat was under the table.", "Fēlēs sub mēnsā erat."},
      {"We have been to Rome.", "Rōmae fuimus."},
      // "where" + be + subject: the place question
      {"Where were you?", "Ubi erās?"},
      {"Where have you been?", "Ubi fuistī?"},
      {"Where were the children?", "Ubi erant puerī?"},
      // a word after a possessive is a noun ("hat", not a past of "hit")
      {"Where is my hat?", "Ubi est pilleus meus?"},
      {"I found his hat.", "Pilleum eius invēnī."},
      // let + someone: jussive of that person; "let me" -> sine mē + infinitive
      {"Let him go.", "Eat."},
      {"Let the children play.", "Puerī lūdant."},
      {"Let me help you.", "Sine mē tē adiuvāre."},
      // a personal pronoun with "all" that is not the subject keeps the pronoun
      {"You can carry us all.", "Nōs omnēs portāre potes."},
      {"I saw you all.", "Vōs omnēs vīdī."},
  });
}

TEST_CASE("rules-f: emoji never on a title or a translated name (work item a)") {
  NEED_REAL();
  for (const char* s : {"The Cowardly Lion is asleep.", "The kind Stork saved me."}) {
    auto e = engine();
    rules::CueInput c;
    c.sourceText = s;
    c.startMs = 0;
    c.endMs = 3000;
    rules::Options o;
    o.speakerGender = 'f';
    auto r = e->translate({c}, o, rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    for (const auto& t : r.value()[0].tokens)
      CHECK_MESSAGE(t.emoji.empty(), s << ": " << t.text << t.emoji);
  }
  // an ordinary noun keeps its emoji
  {
    auto e = engine();
    rules::CueInput c;
    c.sourceText = "The lion is asleep.";
    c.startMs = 0;
    c.endMs = 3000;
    auto r = e->translate({c}, rules::Options{}, rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    bool any = false;
    for (const auto& t : r.value()[0].tokens) any = any || !t.emoji.empty();
    CHECK(any);
  }
}

TEST_CASE("rules-f: core words of children's stories are tier 1/2 (A6), the lower tier wins (work item e)") {
  NEED_REAL();
  // each tier row with two own sentences: the word is chosen and A6 holds
  const std::pair<const char*, const char*> cases[] = {
      {"The maid cleaned the kitchen.", "serua"},        {"My grandmother tells stories.", "auia"},
      {"I visited my grandmother.", "auiam"},             {"The cook made soup.", "coquus"},
      {"The cook was angry.", "coquus"},                  {"The frog jumped.", "rana"},
      {"I saw a green frog.", "ranam"},                   {"The giant was very tall.", "gigas"},
      {"The children feared the giant.", "gigant"},     {"The ice is thin.", "glacies"},
      {"We walked on the ice.", "glacie"},                {"The puppy is sleeping.", "catulus"},
      {"My poor little kitten!", "catul"},               {"He hit the wolf with a stick.", "baculo"},
      {"The old man had a stick.", "baculum"},            {"I looked everywhere.", "ubique"},
      {"Snow lay everywhere.", "ubique"},                 {"We went nowhere.", "nusquam"},
      {"The cat is nowhere.", "nusquam"},                 {"She has a silver ring.", "argente"},
      {"The king had silver cups.", "argente"},           {"The dog ran back.", "retro"},
      {"Don't look back!", "retro"},                      {"The boy was badly hurt.", "grauiter"},
      {"The ship was badly damaged.", "grauiter"},        {"The farmer and his wife are happy.", "uxor"},
      {"The king loved his wife.", "uxorem"},
  };
  for (const auto& c : cases) {
    const std::vector<Out> o = run({c.first});
    CHECK_MESSAGE(norm(o[0].text).find(c.second) != std::string::npos, c.first << " -> " << o[0].text << " (want " << c.second << ")");
    for (const auto& k : o[0].checks)
      if (k.id == "A6") CHECK_MESSAGE(k.ok, c.first << " -> " << o[0].text << ": " << k.detail);
  }
}

TEST_CASE("rules-f: passives (transitive Latin verb, agents, born, intransitive Latin verbs) (C19)") {
  NEED_REAL();
  expectEach({
      // "was hurt" (past = participle) is a passive; the Latin verb of a personal passive takes an accusative
      {"The boy was badly hurt.", "Puer graviter laesus est."},
      {"The boy was hurt.", "Puer laesus est."},
      {"The rope was cut.", "Fūnis incīsus est."},
      // agents: persons and animals of the stories take ā / ab (and ā before v)
      {"The wolf was seen by the hunter.", "Lupus ā vēnātōre vīsus est."},
      {"The princess was saved by a knight.", "Rēgīna ā mīlite servāta est."},
      // be born -> nāscor
      {"She was born in a small village.", "In vīcō parvō nāta est."},   // C22: village -> vīcus (tiers_la.tsv)
      {"The puppies were born yesterday.", "Catulī heri nātī sunt."},
      // a Latin verb without passive forms says the passive actively (never a missing form)
      {"The snow melted.", "Nix licuit."},
      {"The ice will be melted soon.", "Glaciēs mox liquēscet."},
      {"The witch was melted.", "Sāga licuit."},
  });
}

TEST_CASE("rules-f: relative clauses attach to their noun; heavy objects follow the verb (work item f)") {
  NEED_REAL();
  expectEach({
      // a relative after a comma belongs to the noun before it, never "et quis ..."
      {"I love my teacher, who is very kind.", "Magistrum meum quī valdē benignus est amō."},
      {"We saw the old baker, who lives near the river.", "Pānificem veterem quī prope flūmen habitat vīdimus."},
      // without a comma, right after its noun ("people who are honest", "the man who was hungry")
      {"We love people who are honest.", "Amāmus eōs quī honestī sunt."},
      {"Nobody dares to hurt a person who is kind.", "Nēmō audet nocēre eī quī benignus est."},
      // "a person who" -> is quī; the heavy object after the verb group, the modal before its infinitive
      {"No one will help a person who steals.", "Nēmō adiuvābit eum quī fūrātur."},
      {"I know a girl who sings beautifully.", "Puellam quae pulchrē canit nōvī."},
      // phrasebook rows (C19)
      {"How have you been?", "Quōmodo valuistī?"},
      {"How have you been, my friend?", "Quōmodo valuistī, mea amīca?"},
      {"How old are you?", "Quot annōs nāta es?"},
      {"How old are you, little girl?", "Quot annōs nāta es, puella parva?"},
      {"The queen was kind.", "Rēgīna benigna erat."},
  });
  for (const char* s : {"I love my teacher, who is very kind.", "We saw the old baker, who lives near the river."})
    CHECK(run({s})[0].text.find("quis") == std::string::npos);
}

TEST_CASE("rules-f: appositions, garden beds, out of, hide, two-gender nouns (C19)") {
  NEED_REAL();
  expectEach({
      // an apposition to the object between commas: the object's case, after the verb when it has a relative clause
      {"We visited our uncle, the old fisherman, who lives near the sea.",
       "Avunculum nostrum vīsitāvimus piscātōrem veterem quī prope mare habitat."},
      {"You must help my friend, the little mouse, who is hungry.", "Amīcum meum adiuvāre dēbēs mūrem parvum quī ēsurit."},
      // a bed of plants is a plot: ager + genitive plural; a noun used as a modifier is a genitive
      {"The lion is asleep in the poppy bed.", "Leō in agrō papāverum dormit."},
      {"The roses grow in the flower bed.", "Rosae in agrō flōrum crēscunt."},
      // out of -> ex
      {"Get out of this flower bed.", "Ex hōc agrō flōrum exī."},
      {"The mouse came out of its hole.", "Mūs ē foveā suā exiit."},
      // hide: with an object cēlō, without one lateō (phrasal_en_la.tsv)
      {"She hid the key under a stone.", "Clāvem sub saxō cēlāvit."},
      {"Where are you hiding?", "Ubi latēs?"},
      // a noun of two genders whose plural ends in -a agrees as a neuter
      {"The poppies are red.", "Papāvera rubra sunt."},
      {"She picked a red poppy.", "Papāver rubrum carpsit."},
  });
}

TEST_CASE("rules-f: get behind / under, meet, as + clause, emphatic ipse (C19)") {
  NEED_REAL();
  expectEach({
      // verbprep_en_la.tsv rows
      {"Get behind the door!", "Stā post iānuam!"},
      {"Get under the table!", "Subī mēnsam!"},
      {"Quickly, get under the bed!", "Celeriter, subī lectum!"},
      // occurrō + dative (tiers_la.tsv, valency_la.tsv)
      {"I will meet you at the gate.", "Tibi in portā occurram."},
      {"We will meet the king tomorrow.", "Crās rēgī occurrēmus."},
      {"I saw your sister yesterday.", "Heri sorōrem tuam vīdī."},
      // "as" + a clause about the object: the object's present participle
      {"I greeted them as they came.", "Eōs venientēs salūtāvī."},
      {"I will meet them as they come.", "Eīs venientibus occurram."},
      // for / by + reflexive: emphatic ipse (gender of the person addressed or speaking from the speaker glossary)
      {"You must find that out for yourself.", "Id ipsa cognōscere dēbēs."},
      {"I made this cake by myself.", "Hanc placentam ipsa fēcī."},
      {"The boy built the boat by himself.", "Puer nāvem ipse aedificāvit."},
      {"She did it for herself.", "Sibi id fēcit."},
      // eīs (dative / ablative plural of is) as the readers write it
      {"I gave them bread.", "Pānem eīs dedī."},
  });
  // "quam prīmum" is no agreement error
  const std::vector<Out> o = run({"Come home as soon as you can."});
  for (const auto& k : o[0].checks)
    if (k.id == "A3") CHECK_MESSAGE(k.ok, o[0].text << ": " << k.detail);
}

TEST_CASE("rules-f: frame cases from the Greek review (fronted time clauses, -ing phrases, possessives, imperatives)") {
  NEED_REAL();
  expectEach({
      // 1. "When / After / While / Before / As soon as X, Y." is a statement with a time clause, never a question;
      // "rose" / "set" after the subject of such a clause are its verbs (rise of the sun -> orior, set -> occidō)
      {"When the sun rose, we went to the river.", "Cum sōl ortus est, ad flūmen iimus."},
      {"When the moon rose, the wolves howled.", "Cum lūna orta est, lupī ululāvērunt."},
      {"Before the sun set, we were home.", "Antequam sōl occidit, domī erāmus."},
      {"After the rain stopped, the children played outside.", "Postquam pluvia dēsiit, puerī forīs lūsērunt."},
      {"As soon as the bell rang, the boys ran out.", "Ubi tintinnābulum pulsāvit, puerī excurrērunt."},
      {"As soon as the sun rose, the birds sang.", "Ubi sōl ortus est, avēs cecinērunt."},
      // 2. an -ing phrase between commas after the subject: a participle agreeing with the subject, in place
      {"The shepherd, seeing the wolf, fled.", "Pāstor, lupum vidēns, fūgit."},
      {"The girl, hearing a noise, woke up.", "Puella, strepitum audiēns, experrēcta est."},
      {"The boy, holding a lamp, entered the cave.", "Puer, lucernam tenēns, antrum invāsit."},
      // flee -> fugiō (tier 1); run away -> aufugiō
      {"The shepherd fled.", "Pāstor fūgit."},
      {"The thief ran away.", "Fūr aufūgit."},
      // 3. a possessive inside a phrasebook slot is kept ("Ubi est māter tua?")
      {"Where is your mother?", "Ubi est māter tua?"},
      {"Where is her cat?", "Ubi est fēlēs eius?"},
      // 4. an imperative row needs the bare verb; an imperative before a comma stays an imperative
      {"Hurry, the ship is leaving!", "Festīnā, nāvis exit!"},
      {"He hurried home.", "Domum festīnāvit."},
      {"The girl woke up.", "Puella experrēcta est."},
      // an object the parser hung on a time word
      {"We will meet the king tomorrow.", "Crās rēgī occurrēmus."},
      {"I saw your sister yesterday.", "Heri sorōrem tuam vīdī."},
  });
  for (const char* s : {"When the sun rose, we went to the river.", "Hurry, the ship is leaving!", "Run, the bear is coming!"}) {
    const std::vector<Out> o = run({s});
    CHECK_MESSAGE(o[0].text.find("Quandō") == std::string::npos, s << " -> " << o[0].text);
    CHECK_MESSAGE(!hasFlag(o[0], "name-guessed"), s << " -> " << o[0].text);
  }
  // a frame check that the Greek path shares: an imperative before a comma is no name candidate
  frame::FrameBuilder fb(frame::SrcLang::En, &real().pen, &real().en, cur());
  for (const char* s : {"Hurry, the ship is leaving!", "Run, the bear is coming!"}) {
    frame::SemSentence ss;
    fb.analyse(s, ss);
    REQUIRE(!ss.tokens.empty());
    CHECK_MESSAGE(ss.tokens[0].upos != "PROPN", s << ": " << ss.tokens[0].upos);
  }
}

TEST_CASE("rules-f: all (= completely) -> tōtus / omnēs; get = fetch; can after a future (C19)") {
  NEED_REAL();
  expectEach({
      // "all" describing the subject of a passive or of an adjective predicate
      {"The cake was all eaten.", "Placenta tōta ēsa est."},
      {"I am all wet.", "Tōta ūmida sum."},
      {"The children were all tired.", "Puerī omnēs fessī erant."},
      // get + a thing (not from a person, not in the past) is fetching: capiō
      {"Go and get some water.", "Ī et cape aquam."},
      {"Please get the ball from the garden.", "Quaesō cape pilam ab hortō."},
      {"I got a letter from my aunt.", "Epistulam ab amitā meā accēpī."},
      // "can" in a clause joined to a future clause is future (poterimus)
      {"We will win, and then we can rest.", "Vincēmus et tum requiēscere poterimus."},
      {"I will find the key, and then you can open the door.", "Clāvem inveniam et tum iānuam aperīre poteris."},
  });
}

TEST_CASE("rules-f: two plural nouns joined by and stay a coordination; a neuter substantive object (C19)") {
  NEED_REAL();
  for (const auto& c : std::vector<std::pair<const char*, const char*>>{
           {"Only witches and sorceresses wear white.", "sāgae et venēficae"},
           {"and only witches and sorceresses wear white.", "sāgae et venēficae"},
           {"Only kings and queens wear crowns.", "Rēgēs et rēgīnae"}}) {
    const std::vector<Out> o = run({c.first});
    CHECK_MESSAGE(o[0].text.find(c.second) != std::string::npos, c.first << " -> " << o[0].text);
    CHECK_MESSAGE(o[0].conf != rules::Confidence::Fix, c.first << " -> " << o[0].text);
  }
}

TEST_CASE("rules-f: fixes after the blind check (own sentences of children's dialogue, C19)") {
  NEED_REAL();
  expectEach({
      // a possessor ('s) the parser hung on the verb
      {"The fisherman's wife wanted a bigger house.", "Uxor piscātōris domum maiōrem voluit."},
      {"The farmer's dog barked.", "Canis agricolae lātrāvit."},
      // give + person + thing: the person is the indirect object
      {"She gave the poor old man some bread and cheese.", "Senī pauperī pānem et cāseum dedit."},
      {"He gave the hungry dog a bone.", "Canī ieiūnō os dedit."},
      // "may I / we ...?" asks permission: licet + dative + infinitive; a common noun before the comma is addressed
      {"Mother, may I go out?", "Māter, licetne mihi exīre?"},
      {"May we play in the garden?", "Licetne nōbīs in hortō lūdere?"},
      {"It may rain tomorrow.", "Crās fortāsse pluit."},
      // an -s verb after "where the X" (lives, not the plural of life); live = dwell with where / here
      {"Nobody knows where the dragon lives.", "Nēmō scit ubi dracō habitet."},
      {"Tell me where the king lives.", "Dīc mihi ubi rēx habitet."},
      {"We live here.", "Hīc habitāmus."},
      // an adverb in -ly through the Latin adjective's own adverb
      {"The stars are shining brightly.", "Stēllae clārē lūcent."},
      {"The sun shone brightly.", "Sōl clārē lūxit."},
      // "than the first": the adjective agrees with the subject
      {"The second bear was smaller than the first.", "Ursus alter minor erat quam prīmus."},
      {"The second girl was taller than the first.", "Puella altera altior erat quam prīma."},
  });
  CHECK(run({"Grandmother, may we bake a cake?"})[0].text.find("Avia, licetne") == 0);
}

TEST_CASE("rules-f: those who, coordinated relatives, ablative of cause (fragments, C19)") {
  NEED_REAL();
  expectEach({
      {"to those who are poor,", "Eīs quī pauperēs sunt,"},
      {"But to those who are honest,", "Sed eīs quī honestī sunt,"},
      // a second relative clause coordinated with the first keeps the relative pronoun
      {"But to those who are not honest, or who approach him from curiosity,",
       "Sed eīs quī honestī nōn sunt aut quī eī cūriōsitāte appropinquant,"},
      // "from" + a feeling is the cause: a bare ablative
      {"She cried from fear.", "Timōre flēvit."},
      {"He opened the box from curiosity.", "Arcam cūriōsitāte aperuit."},
  });
  const std::vector<Out> o = run({"We thanked those who were kind, or who helped us."});
  CHECK_MESSAGE(o[0].text.find("quis") == std::string::npos, o[0].text);
}

// ================================================================================================================
// C20 (RULES-G, Latin pre-loop). Generalisation guard: every rule below has at least two sentences of our own, written
// before the rule was run on the tuning sample (docs/rules_en_notes.md "Pre-loop C20").
TEST_CASE("rules-g: standalone possessive pronouns agree with the noun they stand for (work item 1)") {
  NEED_REAL();
  expectEach({
      {"My brother is taller than yours.", "Frāter meus altior est quam tuus."},
      {"Your house is bigger than mine.", "Domus tua maior est quam mea."},
      {"Their garden is smaller than ours.", "Hortus eōrum minor est quam noster."},
      {"Our cat is faster than theirs.", "Fēlēs nostra celerior est quam eōrum."},
      {"The red ball is his.", "Pila rubra eius est."},
      {"This book is mine.", "Hic liber meus est."},
  });
  // never a bracketed source word; the compared object keeps its case
  for (const char* s : {"My dog is older than hers.", "I lost my pen, can I use yours?", "Yours is bigger.",
                        "I like your hat more than mine.", "Is this hat yours?"}) {
    const std::vector<Out> o = run({s});
    CHECK_MESSAGE(o[0].text.find('[') == std::string::npos, s << " -> " << o[0].text);
  }
  CHECK(run({"My dog is older than hers."})[0].text.find("quam eius") != std::string::npos);
  CHECK(run({"I lost my pen, can I use yours?"})[0].text.find("tuō ūtī") != std::string::npos);
  CHECK(run({"I like your hat more than mine."})[0].text.find("quam meum") != std::string::npos);
}

TEST_CASE("rules-g: an imperative, a comma and a statement keep the comma (asyndeton, work item 2)") {
  NEED_REAL();
  expectEach({
      {"Run, the dragon is coming!", "Curre, dracō venit!"},
      {"Come quickly, the bread is burning!", "Venī celeriter, pānis ārdet!"},
      {"Hide, the witch is here!", "Latē, sāga hīc est!"},
      {"Run and hide!", "Curre et latē!"},   // a real "and" stays et
  });
  CHECK(run({"Run, the dragon is coming!"})[0].conf == rules::Confidence::Ok);
  // a word the tagger took for an interjection and the lexicon knows as a verb: repaired, so never OK
  CHECK(run({"Hide, the witch is here!"})[0].conf != rules::Confidence::Ok);
  // real interjections are untouched
  CHECK(run({"Oh, the dragon is coming!"})[0].text == "Ō, dracō venit!");
}

TEST_CASE("rules-g: such / so + adjective, so many, so ... that (work item 3)") {
  NEED_REAL();
  expectEach({
      {"I have never seen such a big dog.", "Numquam tantum canem vīdī."},
      {"It was such a beautiful day.", "Tam pulcher diēs erat."},
      {"I have never heard such a noise.", "Numquam tālem strepitum audīvī."},
      {"There were so many apples on the tree.", "In arbore erant tot māla."},
      {"We were so hungry that we ate everything.", "Tam ēsuriēbāmus ut omnia ederēmus."},
      {"The night was so dark that we could not see the road.", "Nox tam obscūra erat ut viam vidēre nōn possēmus."},
      {"She has so many friends that she is never alone.", "Tot amīcōs habet ut numquam sōla sit."},
      {"It was such a big dog that everybody ran away.", "Tantus canis erat ut omnēs aufugerent."},
      {"She ran so fast that nobody could catch her.", "Tam cito cucurrit ut nēmō eam capere posset."},
      // an actual result after a perfect keeps the perfect subjunctive (C17)
      {"He sang so badly that we laughed.", "Tam male cecinit ut rīserīmus."},
  });
  // the degree word is never dropped silently
  for (const char* s : {"The giant had such a loud voice.", "There was so much water in the river.", "I was so afraid."}) {
    const std::vector<Out> o = run({s});
    const bool deg = o[0].text.find("tam") != std::string::npos || o[0].text.find("Tam") != std::string::npos ||
                     o[0].text.find("tant") != std::string::npos || o[0].text.find("Tant") != std::string::npos;
    CHECK_MESSAGE(deg, s << " -> " << o[0].text);
  }
}

TEST_CASE("rules-g: a noun + to-infinitive is a gerundive or ad + gerund, not a relative clause (work item 4)") {
  NEED_REAL();
  expectEach({
      {"Give me a book to read.", "Dā mihi librum legendum."},
      {"I have a letter to write.", "Epistulam scrībendam habeō."},
      {"They had no water to drink.", "Aquam bibendam nōn habēbant."},
      {"She found some bread to eat.", "Pānem ad edendum invēnit."},
  });
  for (const char* s : {"We need a place to sleep.", "They brought us food to eat.", "He gave me a coat to wear."}) {
    const std::vector<Out> o = run({s});
    CHECK_MESSAGE(o[0].text.find(" ad ") != std::string::npos, s << " -> " << o[0].text);
    CHECK_MESSAGE(o[0].text.find(" qu") == std::string::npos, s << " -> " << o[0].text);
  }
}

TEST_CASE("rules-g: my child in address is mī fīlī / mea fīlia; my is never dropped (work item 5)") {
  NEED_REAL();
  expectEach({
      {"Come here, my child.", "Venī hūc, mea fīlia."},
      {"Don't cry, my child.", "Nōlī flēre, mea fīlia."},
      {"My child, where are you?", "Mea fīlia, ubi es?"},
      {"Come here, child.", "Venī hūc, puella."},
      {"Good night, my son.", "Bene dormī, fīlī mī."},
  });
  CHECK(run({"Don't cry, my child."}, 'm')[0].text == "Nōlī flēre, mī fīlī.");
  CHECK(run({"Where are you going, my child?"}, 'm')[0].text == "Quō īs, mī fīlī?");
}

TEST_CASE("rules-g: fixes after the blind check (own sentences of children's dialogue, C20)") {
  NEED_REAL();
  expectEach({
      // "N times" -> numeral adverbs; twice -> bis
      {"She read the book ten times.", "Librum deciēns lēgit."},
      {"I have told you twice.", "Tibi bis dīxī."},
      {"The wolf knocked on the door three times.", "Lupus iānuam ter pulsāvit."},
      // knock on / at -> pulsō + the object (verbprep_en_la.tsv)
      {"Someone is knocking at the gate.", "Aliquis portam pulsat."},
      // "until" + a past event -> dōnec + perfect; the past form closing the clause is its verb (rose, not a rose)
      {"We sang songs until the moon rose.", "Carmina cecinimus dōnec lūna orta est."},
      {"Wait here until I come back.", "Manē hīc dum redeō."},
      // something / anything + to-infinitive -> ad + gerund
      {"Is there anything to drink?", "Estne aliquid ad bibendum?"},
      // lost: the participle of āmittō (teacher gloss), also after a possessive and when tagged as a finite verb
      {"The lost boy was crying.", "Puer āmissus flēbat."},
      {"I lost my hat.", "Pilleum meum āmīsī."},
      {"She found her lost ring.", "Ānulum āmissum suum invēnit."},
  });
  CHECK(run({"We played until the bell rang."})[0].text.find("dōnec") != std::string::npos);
  CHECK(run({"Give me something to eat, please."})[0].text.find("aliquid ad edendum") != std::string::npos);
  CHECK(run({"If you are good, you will get a present."})[0].text.find("dōnum") != std::string::npos);
  CHECK(run({"Thank you for the lovely present."})[0].text.find("dōnō") != std::string::npos);
  CHECK(run({"The old man knocked on the window."})[0].text.find("fenestram pulsāvit") != std::string::npos);
  // a deponent's perfect participle is active: never the passive "stolen"
  for (const char* s : {"The queen found her stolen crown.", "The stolen horse came back."})
    CHECK_MESSAGE(run({s})[0].text.find("fūrāt") == std::string::npos, s);
  // reported statements: the perfect passive infinitive agrees with its accusative subject; an object pronoun that is
  // the speaker is the reflexive
  expectEach({
      {"I know that the gate was closed.", "Sciō portam clausam esse."},
      {"The queen said that the letters were written.", "Rēgīna dīxit epistulās scrīptās esse."},
      {"He said that the girl had followed him.", "Dīxit puellam sē secūtam esse."},
      {"She said that the boys had helped her.", "Dīxit puerōs sē adiūvisse."},
  });
  CHECK(run({"I said that the girl had seen him."})[0].text.find(" sē ") == std::string::npos);
}

// ================================================================================================================
// C22 (RULES-H, acceptance loop 1). Generalisation guard: every rule below has at least two sentences of our own
// (never lines of the acceptance file), written before the rule was run on that file (docs/rules_en_notes.md
// "Acceptance loop 1 (C22)").
namespace {
bool hasBracket(const std::string& s) { return s.find('[') != std::string::npos; }
}  // namespace

TEST_CASE("rules-h: no cue is emptied or swallowed by the next one (quotes, ellipsis, stray bar)") {
  NEED_REAL();
  {
    const std::vector<Out> o = run({"- \"The farmer's dog at first was... \"", "Mary."});
    REQUIRE(o.size() == 2);
    CHECK(o[1].text == "Marīa.");
    CHECK(o[0].text.find("Marīa") == std::string::npos);
  }
  {
    const std::vector<Out> o = run({"\"The baker had been sleeping for... \"", "Lucy!"});
    REQUIRE(o.size() == 2);
    CHECK(o[1].text == "Lūcia!");
    CHECK_FALSE(o[0].text.empty());
  }
  {
    const std::vector<Out> o = run({"The dog was sleeping.|", "Goodbye, Tom. Goodbye!"});
    REQUIRE(o.size() == 2);
    CHECK(o[0].text == "Canis dormiēbat.");
    CHECK(o[1].text.rfind("Valē", 0) == 0);
  }
  // every cue of a multi-cue sentence keeps Latin words of its own
  for (const auto& seq : std::vector<std::vector<std::string>>{
           {"\"The old king, the father of the prince,", "gave him a horse and a sword. \""},
           {"We walked to the river and", "sat under a tree."}}) {
    const std::vector<Out> o = run(seq);
    for (size_t i = 0; i < o.size(); ++i) CHECK_MESSAGE(!o[i].text.empty(), seq[i]);
  }
}

TEST_CASE("rules-h: names of the table, places of two words, titles before a name (work item a)") {
  NEED_REAL();
  expectEach({
      {"Lucy in the garden", "Lūcia in hortō"},
      {"We live in Wonderland.", "In Terrā Mīrābilī habitāmus."},
      {"Where is the bridge over the river?", "Ubi est pōns super flūmen?"},
      {"Because my garden would be a Wonderland", "Quia hortus meus Terra Mīrābilis esset"},
      {"The moon goes round the Earth.", "Lūna circum Terram it."},
      {"Mr. Fox, wait!", "Vulpēs, manē!"},
      {"Yes, Miss Lucy.", "Ita, Lūcia."},
      {"Mr. Bear!", "Urse!"},
  });
  // "Mr." is never written in the Latin, a known name never in brackets
  for (const char* s : {"Mr. Fox. Wait!", "Lucy!", "Good morning, Mr. Bear."}) {
    const std::vector<Out> o = run({s});
    CHECK_MESSAGE(o[0].text.find("Mr") == std::string::npos, s << " -> " << o[0].text);
    CHECK_MESSAGE(!hasBracket(o[0].text), s << " -> " << o[0].text);
  }
}

TEST_CASE("rules-h: function words, greetings and table words never in brackets (work item b)") {
  NEED_REAL();
  expectEach({
      {"I am lonely.", "Sōla sum."},
      {"The old man was very lonely.", "Senex valdē sōlus erat."},
      {"She bought a dozen apples.", "Duodecim māla ēmit."},
      {"I saw two dozen eggs.", "Vīgintī quattuor ōva vīdī."},
      {"That is nonsense.", "Nūgae sunt."},
      {"What nonsense, Tom!", "Quae nūgae, Tom!"},
      {"Contrariwise, the dog was happy.", "Contrā canis laetus erat."},
      {"The man wore a red waistcoat.", "Vir subūculam rubram gessit."},
      {"Within the castle we were safe.", "Intrā castrum tūtae erāmus."},
      {"Under the table or here or there", "Sub mēnsā aut hīc aut illīc"},
      {"Over the bridge or here or there", "Super pontem aut hīc aut illīc"},
      {"Say hello to grandmother.", "Salūtā aviam."},
      {"We said goodbye to our friends.", "Amīcīs nostrīs valedīximus."},
      {"The bear has thick fur.", "Ursus pellem crassam habet."},
      {"I have a watch.", "Hōrologium habeō."},
  });
  for (const char* s : {"She said hello to me.", "I must say goodbye.", "The how-do-you-do frogs sang.",
                        "Each child would have a dozen bluebirds and a cake.", "We saw three bluebirds in the tree.", "I'm overdue."}) {
    const std::vector<Out> o = run({s});
    CHECK_MESSAGE(!hasBracket(o[0].text), s << " -> " << o[0].text);
  }
}

TEST_CASE("rules-h: unreal conditions and would: imperfect subjunctive throughout (work item c)") {
  NEED_REAL();
  expectEach({
      {"If I had a dog, I would be happy.", "Sī canem habērem, laeta essem."},
      {"I would be happy if I had a dog.", "Laeta essem sī canem habērem."},
      {"If we had wings, we would fly to the moon.", "Sī ālās habērēmus, ad lūnam volārēmus."},
      {"The mice would sit and drink with us.", "Mūrēs sedērent et nōbīscum biberent."},
      {"He will come and help us.", "Veniet et nōs adiuvābit."},
      {"There'd be flowers everywhere.", "Ubīque essent flōrēs."},
      {"There would be new flowers", "Essent flōrēs novī"},
      {"There will be cake for everyone.", "Omnibus erit placenta."},
  });
  // "you would" alone stands for the verb before it (a guess: Check)
  {
    const std::vector<Out> o = run({"She would not sing.", "But he would."});
    CHECK(o[1].text == "Sed caneret.");
    CHECK(o[1].conf != rules::Confidence::Ok);
  }
  {
    const std::vector<Out> o = run({"She will not come.", "But he will."});
    CHECK(o[1].text == "Sed veniet.");
  }
  // a song line "And + bare verb" goes on with the line before it
  {
    const std::vector<Out> o = run({"\xE2\x99\xAA The dogs would sit by the fire", "\xE2\x99\xAA And sing all night"});
    CHECK(o[1].text == "\xE2\x99\xAA Et tōtam noctem canerent");
  }
  {
    const std::vector<Out> o = run({"\xE2\x99\xAA We could climb the mountain", "\xE2\x99\xAA And see the sea"});
    CHECK(o[1].text == "\xE2\x99\xAA Et mare vidēre poterāmus");
  }
  {
    const std::vector<Out> o = run({"\xE2\x99\xAA The frogs would live in tiny boats", "\xE2\x99\xAA And be fed with honey and cake"});
    CHECK(o[1].text == "\xE2\x99\xAA Et melle et placentā alerentur");
  }
  // subject and predicate noun of a copula need not share the gender (no A3 Fix)
  for (const char* s : {"Because my house would be a palace", "The world would be a garden.",
                        "Then everything would be toys."})
    CHECK_MESSAGE(run({s})[0].conf != rules::Confidence::Fix, s);
  expectEach({
      {"The boys promised to wash the dog and feed the cat.",   // C28: promise + accusative and future infinitive
       "Puerī canem sē lavātūrōs esse prōmīsērunt et fēlem sē altūrōs esse prōmīsērunt."},
      {"Robert agreed to meet the king and give him the ring.", "Rōbertus rēgī occurrere cōnsēnsit et ānulum eī dare cōnsēnsit."},
      {"She wants to sing and dance.", "Canere vult et saltāre vult."},
      {"I wish it was always summer.", "Optō ut semper aestās sit."},   // C24: primary sequence after optō
      {"She wishes that he would come.", "Optat ut veniat."},
      {"I wish I could fly.", "Optō ut volāre possim."},
  });
}

TEST_CASE("rules-h: idioms and phrases of children's dialogue (work item d)") {
  NEED_REAL();
  expectEach({
      {"Pay attention to the teacher!", "Attende animum ad magistrum!"},
      {"You must pay attention to your book.", "Animum ad librum tuum attendere dēbēs."},
      {"I'm late for dinner.", "Ad cēnam sērō veniō."},
      {"We are late for school.", "Ad lūdum sērō venīmus."},
      {"We're late, late, late!", "Sērō venīmus, sērō, sērō!"},
      {"Oh, I'm in a stew!", "Ō, perturbāta sum!"},
      {"That's it, children.", "Ita est, puerī."},
      {"Read it once more, from the beginning.", "Lege id rūrsus, ab initiō."},
      {"After all, he is only a boy.", "Nam, puer tantum est."},
      {"I keep wishing for snow.", "Semper nivem volō."},
      {"I want a house of my own.", "Domum meam volō."},
      {"She has a room of her own.", "Cubiculum suum habet."},
      {"We played in a garden of our own.", "In hortō nostrō lūsimus."},
      {"She lives in a little room of her own.", "In cubiculō parvō suō habitat."},
      {"He sings just like a bird.", "Sīcut avis canit."},
      {"She is like her mother.", "Mātrī suae similis est."},
      {"And all the other children, too.", "Et cēterī puerī quoque."},
      {"And the cats, too.", "Et fēlēs quoque."},
      {"I want some cake, too.", "Placentam quoque volō."},
      {"This letter is very important.", "Haec epistula maximī mōmentī est."},
      {"The king has an important letter.", "Rēx epistulam magnī mōmentī habet."},
      {"My ears and paws!", "Ō aurēs et pedēs meōs!"},
      {"My poor cat!", "Ō fēlem pauperem meam!"},
      {"The road was...", "Via erat..."},
      {"At first the dog was...", "Prīmō canis erat..."},
      {"She thinks nothing of swimming in the lake.", "Nihil cūrat in lacū nāre."},
      {"What a strange place to build a house!", "Quam mīrus locus domūs aedificandae!"},
  });
    CHECK(run({"The letter must be awfully important."})[0].text == "Epistula maximī mōmentī esse dēbet.");
  CHECK(run({"Sing it once more."})[0].text.find("rūrsus") != std::string::npos);
}

TEST_CASE("rules-h: adjective after the copula, said once; too = quoque; generic one (work item f)") {
  NEED_REAL();
  expectEach({
      {"That's silly.", "Id stultum est."},
      {"This is very silly.", "Hoc valdē stultum est."},
      {"She has very extra special shoes.", "Calceōs valdē praecipuōs habet."},
      {"How can one read in the dark?", "Quōmodo homo in tenebrīs legere potest?"},
      {"This is my cat.", "Haec fēlēs mea est."},
      {"That is a big dog.", "Canis magnus est."},
      {"What a big house!", "Quam magna domus!"},
  });
  for (const char* s : {"I want some cake, too.", "And the cats, too."})
    CHECK_MESSAGE(run({s})[0].text.find("nimis") == std::string::npos, s);
  CHECK(run({"One must eat to live."})[0].text.rfind("Homo", 0) == 0);
}

TEST_CASE("rules-h: the person addressed: a feminine name in the cue or the cue before (work item g)") {
  NEED_REAL();
  {
    const std::vector<Out> o = run({"Lucy.", "My dear child, you are tired."});
    CHECK(o[1].text == "Puella cāra mea, fessa es.");
  }
  {
    const std::vector<Out> o = run({"Lucy.", "My dear child, you are tired."}, 'm');
    CHECK(o[1].text == "Puella cāra mea, fessa es.");   // the name decides, not the speaker setting
  }
  expectEach({
      {"Are you tired, Lucy?", "Esne fessa, Lūcia?"},   // C28: a question with the person addressed last keeps -ne
      {"You are very clever, Mary!", "Valdē callida es, Marīa!"},
  });
  CHECK(run({"You are very clever, Mary!"}, 'm')[0].text == "Valdē callida es, Marīa!");
  // without a name the gender of "you" is a guess: never OK
  CHECK(run({"You are very kind."})[0].conf != rules::Confidence::Ok);
  CHECK(run({"You are very clever!"})[0].conf != rules::Confidence::Ok);
  CHECK(run({"You were wrong."})[0].conf == rules::Confidence::Ok);   // errābās: no gender to guess
}

TEST_CASE("rules-h: song lines and quoted narrative (work items e, h)") {
  NEED_REAL();
  expectEach({
      {"Under the moon and over the sea", "Sub lūnā et super mare"},
      {"In the garden and in the house.", "In hortō et in domō."},
      {"When the boats go sailing by", "Ubi nāvēs nāvigant"},
      {"The boats go sailing by.", "Nāvēs nāvigant."},
      {"Anna, the daughter of the baker, wanted to help us.", "Anna, fīlia pānificis, nōs adiuvāre voluit."},
      {"We met Tom, the son of the baker.", "Tom, fīliō pānificis, occurrimus."},
      {"The two brothers, the sons of the miller, fought for the king.", "Duo frātrēs, fīliī molīnāriī, prō rēge pugnāvērunt."},
      {"In my dream the trees were nothing but flowers.", "In somniō meō arborēs nihil erant nisi flōrēs."},
      {"In our town the houses would be nothing but castles.", "Domūs in oppidō nostrō nihil essent nisi castra."},
      {"He eats nothing but bread.", "Nihil nisi pānem edit."},
      {"He came with no shoes.", "Sine calceīs vēnit."},
      {"It's only a cat with a bell.", "Fēlēs tantum est cum tintinnābulō."},
      {"It is only a fox with a hat and a stick.", "Vulpēs tantum est cum pilleō et baculō."},
      {"Will you kindly close the window, Tom.", "Claude fenestram, quaesō, Tom."},
      {"Would you please sit down?", "Cōnsīde, quaesō."},
      {"What if it rains tomorrow?", "Quid sī crās pluat?"},
      {"What if we should lose the key?", "Quid sī clāvem āmittāmus?"},
  });
  // a when-line without its main clause and a past verb without its subject are fragments: never OK, never orders
  CHECK(run({"When the boats go sailing by"})[0].conf != rules::Confidence::Ok);
  {
    const std::vector<Out> o = run({"... wanted more bread."});
    CHECK(o[0].text.find("voluit") != std::string::npos);
    CHECK(o[0].conf != rules::Confidence::Ok);
  }
}

TEST_CASE("rules-h: fixes after the blind check (own sentences, C22)") {
  NEED_REAL();
  expectEach({
      // an order to a person: the adjective agrees with the person addressed, never neuter
      {"Don't be lazy, Mary.", "Ignāva nōlī esse, Marīa."},
      {"Don't be silly!", "Stultus nōlī esse!"},
      // an unreal condition with "could": imperfect subjunctive of possum
      {"If the cat could fly, it would catch birds.", "Sī fēlēs volāre posset, avēs caperet."},
      {"If I could swim, I would go to the island.", "Sī nāre possem, ad īnsulam īrem."},
      // "every morning / evening" in the past is a habit: imperfect
      {"Every evening she sang to the children.", "Omnī vespere puerīs canēbat."},
      {"Every day they played in the garden.", "Omnī diē in hortō lūdēbant."},
      // late for X again; very much
      {"I am late for the bus again.", "Iterum ad lāophorīum sērō veniō."},
      {"He loved his dog very much.", "Canem suum valdē amābat."},
  });
  CHECK(run({"Don't be silly!"})[0].conf != rules::Confidence::Ok);   // the gender of the person is a guess
  // "with no X in it": in it repeats the noun; a teacher's tier-1 noun is a noun for the checker (mundus world)
  expectEach({
      {"Who wants a box with no toys in it?", "Quis arcam sine lūdibriīs vult?"},
      {"In our world the books are nothing but songs.", "In mundō nostrō librī nihil sunt nisi carmina."},
  });
  CHECK(run({"Our world is a garden."})[0].conf != rules::Confidence::Fix);
}

// ================================================================================================================
// C23 (RULES-I, D18): the latinity toggle. "wide" (default) accepts Medieval / ecclesiastical Latin: tagged senses
// compete without the build-time penalty and phrasebook rows marked eccl are used; "classical" keeps the penalty,
// skips the eccl rows and marks a choice that still lands on a tagged sense Check (flag late-latin). Own sentences.
namespace {
struct LatOut { std::string text; rules::Confidence conf; rules::CueOutput full; };
std::vector<LatOut> runLat(const std::vector<std::string>& src, rules::Latinity lat, char gender = 'f', int fidelity = 2,
                           rules::Lang lang = rules::Lang::En, rules::Engine* eng = nullptr) {
  static std::unique_ptr<rules::Engine> shared = engine();
  rules::Engine* e = eng ? eng : shared.get();
  std::vector<rules::CueInput> in;
  for (size_t i = 0; i < src.size(); ++i) {
    rules::CueInput c;
    c.index = (uint32_t)i;
    c.sourceText = src[i];
    c.startMs = (int64_t)i * 4000;
    c.endMs = c.startMs + 3500;
    in.push_back(c);
  }
  rules::Options o;
  o.source = lang;
  o.speakerGender = gender;
  o.fidelity = fidelity;
  o.latinity = lat;
  auto r = e->translate(in, o, rules::Context{}, nullptr, nullptr);
  REQUIRE(r.ok());
  std::vector<LatOut> out;
  for (const auto& c : r.value()) out.push_back(LatOut{flat(c.target), c.confidence, c});
  return out;
}
bool hasAlt(const rules::CueOutput& c, const std::string& text) {
  for (const auto& a : c.alternatives)
    if (flat(a.text) == text) return true;
  return false;
}
bool flagged(const rules::CueOutput& c, const char* f) { return std::find(c.flags.begin(), c.flags.end(), f) != c.flags.end(); }
std::string tokenRegister(const rules::CueOutput& c, const std::string& word) {
  for (const auto& t : c.tokens)
    if (text::latin_key(t.text) == text::latin_key(word)) return t.registerTag;
  return "?";
}
constexpr rules::Latinity kWide = rules::Latinity::Wide, kClassical = rules::Latinity::Classical;
}  // namespace

TEST_CASE("rules-i: phrasebook register eccl: loader, matcher (classical skips the rows)") {
  // the loader reads "eccl" and "eccl+<register>"; the shipped rows
  bool sorry = false, sorryClassical = false;
  for (const curated::PhraseEntry& e : cur().phrasebook()) {
    if (e.pattern != "i am sorry") continue;
    if (e.eccl) {
      sorry = true;
      CHECK(e.reg == "polite");
      CHECK(e.latin == "habeās mē excūsātum/excūsātam");
      CHECK_FALSE(sorryClassical);   // the eccl row comes first: it is the one chosen with "wide"
    } else {
      sorryClassical = true;
      CHECK(e.latin == "ignōsce mihi");
    }
  }
  CHECK(sorry);
  CHECK(sorryClassical);
  bool es = false;
  for (const curated::PhraseEntry& e : cur().phrasebookEs()) es = es || (e.pattern == "lo siento" && e.eccl);
  CHECK(es);
  frame::Phrasebook pb;
  std::vector<curated::PhraseEntry> rows = {{"we are late", "sērō venīmus", "state", "", 1, true},
                                            {"we are late", "tardē venīmus", "state", "", 1, false},
                                            {"god bless you", "deus tē benedīcat", "excl", "", 1, true}};
  pb.build(rows, cur().contractions());
  auto toks = [](std::initializer_list<const char*> w) {
    std::vector<nlp::Token> v;
    for (const char* x : w) {
      nlp::Token t;
      t.text = x;
      t.lower = text::lower(x);
      t.upos = "X";
      t.lemma = t.lower;
      v.push_back(t);
    }
    return v;
  };
  frame::PhraseMatch m;
  REQUIRE(pb.match(toks({"We", "are", "late"}), 0, m));
  CHECK(m.entry == 0);   // ties: the earlier row
  REQUIRE(pb.match(toks({"We", "are", "late"}), 0, m, true));
  CHECK(m.entry == 1);   // classical: the eccl row is skipped
  CHECK(pb.match(toks({"God", "bless", "you"}), 0, m));
  CHECK_FALSE(pb.match(toks({"God", "bless", "you"}), 0, m, true));
}

TEST_CASE("rules-i: I'm sorry / lo siento, goodbye, thank God, please by latinity (own sentences)") {
  NEED_REAL();
  // wide (default): the owner's ecclesiastical "Habeās mē excūsātum/-am" chosen, Ignōsce mihi offered
  {
    const auto w = runLat({"I'm sorry.", "I'm sorry, Mother."}, kWide, 'f');
    CHECK(w[0].text == "Habeās mē excūsātam.");
    CHECK(hasAlt(w[0].full, "Ignōsce mihi."));
    CHECK(w[0].conf == rules::Confidence::Ok);   // no Check for the register
    CHECK_FALSE(flagged(w[0].full, "late-latin"));
    CHECK(w[1].text == "Habeās mē excūsātam, māter.");
    bool eccl = false;
    for (const auto& r : w[0].full.reasons) eccl = eccl || (r.kind == "phrasebook" && r.data.find("\"register\":\"eccl\"") != std::string::npos);
    CHECK(eccl);
    CHECK(runLat({"I'm sorry."}, kWide, 'm')[0].text == "Habeās mē excūsātum.");
    CHECK(rules::Options{}.latinity == kWide);   // the default
  }
  // classical: the classical row, no ecclesiastical alternative
  {
    const auto c = runLat({"I'm sorry.", "I'm sorry, Mother."}, kClassical, 'f');
    CHECK(c[0].text == "Ignōsce mihi.");
    CHECK_FALSE(hasAlt(c[0].full, "Habeās mē excūsātam."));
    CHECK(c[1].text == "Ignōsce mihi, māter.");
  }
  // goodbye: Valē first in both modes; Deus tē servet only as the alternative, only with "wide"
  {
    const auto w = runLat({"Goodbye."}, kWide);
    CHECK(w[0].text == "Valē.");
    CHECK(hasAlt(w[0].full, "Deus tē servet."));
    const auto c = runLat({"Goodbye."}, kClassical);
    CHECK(c[0].text == "Valē.");
    CHECK_FALSE(hasAlt(c[0].full, "Deus tē servet."));
  }
  // thank God: Deō grātiās (wide), the classical row otherwise; please stays quaesō
  CHECK(runLat({"Thank God!"}, kWide)[0].text == "Deō grātiās!");
  CHECK(runLat({"Thank God!"}, kClassical)[0].text == "Deō grātiās agō!");
  CHECK(runLat({"Please."}, kWide)[0].text == "Quaesō.");
  CHECK(runLat({"Please."}, kClassical)[0].text == "Quaesō.");
  // Spanish mirrors
  if (real().esOk) {
    const auto w = runLat({"Lo siento.", "Adiós.", "¡Gracias a Dios!"}, kWide, 'm', 2, rules::Lang::Es);
    CHECK(w[0].text == "Habeās mē excūsātum.");
    CHECK(hasAlt(w[0].full, "Ignōsce mihi."));
    CHECK(w[1].text == "Valē.");
    CHECK(hasAlt(w[1].full, "Deus tē servet."));
    CHECK(w[2].text == "Deō grātiās!");
    const auto c = runLat({"Lo siento.", "Adiós.", "¡Gracias a Dios!"}, kClassical, 'm', 2, rules::Lang::Es);
    CHECK(c[0].text == "Ignōsce mihi.");
    CHECK(c[1].text == "Valē.");
    CHECK_FALSE(hasAlt(c[1].full, "Deus tē servet."));
    CHECK(c[2].text == "Deō grātiās agō!");
  }
}

TEST_CASE("rules-i: a lemma whose only sense is Medieval: Check in classical, OK-capable in wide") {
  NEED_REAL();
  // alchēmista (Medieval Latin only, the one Latin candidate for "alchemist"); fidelity 1 so the tier ceiling (A6)
  // does not speak
  const std::vector<std::string> src = {"The alchemist is here.", "Where is the alchemist?"};
  const auto w = runLat(src, kWide, 'f', 1);
  const auto c = runLat(src, kClassical, 'f', 1);
  for (size_t i = 0; i < src.size(); ++i) {
    CHECK(w[i].text == c[i].text);   // no classical candidate: the same word, only the mark differs
    CHECK_MESSAGE(w[i].conf == rules::Confidence::Ok, src[i] << " -> " << w[i].text);
    CHECK_FALSE(flagged(w[i].full, "late-latin"));
    CHECK(c[i].conf == rules::Confidence::Check);
    CHECK(flagged(c[i].full, "late-latin"));
    bool hint = false;
    for (const auto& r : c[i].full.reasons) hint = hint || r.text.find("classical Latin was asked for") != std::string::npos;
    CHECK(hint);
    // the chosen sense's register is on the token in both modes (the UI's "late Latin" badge)
    CHECK(tokenRegister(w[i].full, "alchēmista") == "medieval");
    CHECK(tokenRegister(c[i].full, "alchēmista") == "medieval");
  }
  CHECK(w[0].text == "Alchēmista hīc est.");
  // a word of classical Latin carries no register
  CHECK(tokenRegister(runLat({"The girl is here."}, kWide)[0].full, "puella") == "");
}

TEST_CASE("rules-i: a Medieval sense competes as an equal in wide and keeps its penalty in classical") {
  NEED_REAL();
  // "knight": mīles in its Medieval sense ("knight") vs eques; the build-time -30 decides only in classical
  transfer::Transfer tr(real().la, cur());
  transfer::Settings st;
  st.fidelity = 1;
  transfer::Choice cw, cc;
  const uint32_t wide = tr.select("knight", feat::Noun, {}, false, false, st, cw);
  st.classical = true;
  const uint32_t classical = tr.select("knight", feat::Noun, {}, false, false, st, cc);
  REQUIRE(wide != transfer::kNone);
  REQUIRE(classical != transfer::kNone);
  CHECK(morph::cleanHead(real().la.lemma(wide).head) == "mīles");
  CHECK(cw.registerTag == "medieval");
  CHECK(cw.candidates[0].why.find("late Latin accepted") != std::string::npos);
  CHECK(morph::cleanHead(real().la.lemma(classical).head) == "eques");
  CHECK(cc.registerTag.empty());
  for (const auto& k : cc.candidates) CHECK(k.why.find("late Latin accepted") == std::string::npos);
  CHECK(runLat({"The knight is here."}, kWide, 'f', 1)[0].text == "Mīles hīc est.");
  CHECK(runLat({"The knight is here."}, kClassical, 'f', 1)[0].text == "Eques hīc est.");
}

TEST_CASE("rules-i: deterministic per mode (two fresh engines, byte-identical), and the modes differ") {
  NEED_REAL();
  const std::vector<std::string> src = {"I'm sorry.", "Goodbye, my friend.", "Thank God, the boy is safe.",
                                        "The knight is here.", "The alchemist is here.", "Please sit down."};
  for (rules::Latinity lat : {kWide, kClassical}) {
    auto e1 = engine();
    auto e2 = engine();
    const auto a = runLat(src, lat, 'u', 1, rules::Lang::En, e1.get());
    const auto b = runLat(src, lat, 'u', 1, rules::Lang::En, e2.get());
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
      CHECK(a[i].full.target == b[i].full.target);
      CHECK(a[i].conf == b[i].conf);
      CHECK(a[i].full.flags == b[i].full.flags);
      REQUIRE(a[i].full.alternatives.size() == b[i].full.alternatives.size());
      for (size_t k = 0; k < a[i].full.alternatives.size(); ++k)
        CHECK(a[i].full.alternatives[k].text == b[i].full.alternatives[k].text);
    }
  }
  const auto w = runLat(src, kWide, 'u', 1);
  const auto c = runLat(src, kClassical, 'u', 1);
  CHECK(w[0].text != c[0].text);
  CHECK(w[3].text != c[3].text);
  CHECK(w[5].text == c[5].text);
}

// ================================================================================================================
// C24 (RULES-J, acceptance loop 2). Generalisation guard: every rule below has at least two sentences of our own, of
// the shape of the fault and unrelated in wording to the tuning file (docs/rules_en_notes.md "Acceptance loop 2").
TEST_CASE("rules-j: the speaker of a reply is the person addressed in the cue before (work item a)") {
  NEED_REAL();
  {
    const std::vector<Out> o = run({"Grandfather, are you tired?", "Yes, I am very tired."}, 'f');
    CHECK(o[0].text == "Ave, esne fessus?");
    CHECK(o[1].text == "Ita, valdē fessus sum.");
    CHECK(hasFlag(o[1], "speaker-gender"));
    CHECK(o[1].conf != rules::Confidence::Ok);
  }
  {
    const std::vector<Out> o = run({"Are you angry, Paul?", "No, I am not angry."}, 'f');
    CHECK(o[1].text == "Nōn, īrātus nōn sum.");
    CHECK(hasFlag(o[1], "speaker-reply"));
  }
  {   // the other way round: a masculine project speaker answering for Mary
    const std::vector<Out> o = run({"Are you tired, Mary?", "Yes, I am very tired."}, 'm');
    CHECK(o[0].text == "Esne fessa, Marīa?");   // C28: -ne (the question was read as a statement)
    CHECK(o[1].text == "Ita, valdē fessa sum.");
  }
  // a dash turn after a turn that addressed someone
  // (C28: the speaker dashes are kept)
  CHECK(run({"- Peter, are you ready? - Yes, I am ready."}, 'f')[0].text == "- Petre, esne parātus? - Ita, parātus sum.");
  // no reply after a goodbye, and nothing to flag when the genders agree
  CHECK(run({"Goodbye, Peter.", "I am tired now."}, 'f')[1].text == "Nunc fessa sum.");
  {
    const std::vector<Out> o = run({"That's it, Mary.", "I am so happy today."}, 'f');
    CHECK(o[1].text == "Hodiē tam laeta sum.");
    CHECK(!hasFlag(o[1], "speaker-gender"));
  }
  // a noun of address of known sex sets the gender of "you"; a kinship word before a comma is no verb
  CHECK(run({"Mother, are you tired?"}, 'm')[0].text == "Māter, esne fessa?");
  // whole-phrase m/f alternatives only when the two sides are parallel word by word
  CHECK(run({"My friend, come here."}, 'm')[0].text == "Mī amīce, venī hūc.");
  CHECK(run({"My friend, come here."}, 'f')[0].text == "Mea amīca, venī hūc.");
}

TEST_CASE("rules-j: long quoted narrative sentences: reroot, cue split, clause split (work item b)") {
  NEED_REAL();
  CHECK(run({"\"Even Paul, the Bishop of Rome, promised to visit the duke and bring him a horse.\""})[0].text ==
        "Etiam Paulus, Episcopus Rōmae, ducem sē vīsitātūrum esse prōmīsit et equum eī sē lātūrum esse prōmīsit.");   // C28
  {
    const std::vector<Out> o = run({"\"Even Paul, the Bishop of Rome,", "promised to visit the duke and bring him a horse.\""});
    CHECK(o[0].text == "Etiam Paulus, Episcopus Rōmae,");
    CHECK(o[1].text == "ducem sē vīsitātūrum esse prōmīsit et equum eī sē lātūrum esse prōmīsit.");   // C28
    for (const Out& x : o) CHECK(x.conf != rules::Confidence::Fix);
  }
  // a sentence the parser cannot build whole: clause by clause, never the word-by-word Fix
  for (const char* s : {"Jump and run and away they go!", "Hop and skip and away we go!"}) {
    const Out x = run({s})[0];
    CHECK_MESSAGE(!hasFlag(x, "could-not-parse"), s);
    CHECK_MESSAGE(hasFlag(x, "clause-split"), s);
  }
}

TEST_CASE("rules-j: free relatives with what: quod + clause, the antecedent understood (work item c)") {
  NEED_REAL();
  expectEach({
      {"Nothing is what it seems.", "Nihil est quod vidētur."},
      {"The sea would be what the sky is.", "Mare esset quod caelum est."},
      {"What he seems, he is not.", "Quod vidētur nōn est."},
      {"I like what I see.", "Amō quod videō."},
      {"What you have, you keep.", "Quod habēs tenēs."},
      {"She does what she wants.", "Facit quod vult."},
      {"What the cat won't eat, the dog will.", "Canis quod fēlēs nōn edet edet."},
      {"Everything is what it seems because nobody is what he says.", "Omnia sunt quod videntur quia nēmō est quod dīcit."},
      {"I know what you want.", "Sciō quid velīs."},   // after know: an indirect question, unchanged
  });
  CHECK(hasFlag(run({"Nothing is what it seems."})[0], "free-relative"));
  // "seem" is the passive of videō
  expectEach({{"The house seems big.", "Domus magna vidētur."}, {"You seem tired.", "Fessus vidēris."}});
}

TEST_CASE("rules-j: sequence of tenses after a verb of wishing (work item d)") {
  NEED_REAL();
  expectEach({
      {"I wish I could fly.", "Optō ut volāre possim."},
      {"She wishes that the snow could stay.", "Optat ut nix manēre possit."},
      {"She wished that he would come.", "Optāvit ut venīret."},
  });
}

TEST_CASE("rules-j: word choices of songs and stories (work item e)") {
  NEED_REAL();
  expectEach({
      {"The weeks roll by.", "Septimānae praetereunt."},
      {"The wheels roll by.", "Rotae praetereunt."},
      {"The mist rolls away.", "Nebula āvolat."},
      {"The birds leave the nest.", "Avēs nīdum relinquunt."},
      {"Hurry, the ship is leaving!", "Festīnā, nāvis exit!"},
      {"The babbling water is cold.", "Aqua murmurāns frīgida est."},
      {"We heard a roaring lion.", "Leōnem rudentem audīvimus."},
      {"Can you understand me?", "Potesne mē intellegere?"},
      {"I do not understand the question.", "Rogātiōnem nōn intellegō."},
      {"The boy fell down the steps.", "Puer dē gradibus cecidit."},
      {"Don't run down the steps!", "Dē gradibus nōlī currere!"},
      {"The children ran down the hill.", "Puerī dē colle cucurrērunt."},
      {"The picture is upside down.", "Pictūra capite deorsum est."},
      {"Bats sleep upside down.", "Vespertīliōnēs dormiunt capite deorsum."},
      {"He is in trouble.", "In perīculō est."},
      {"We play in the afternoon.", "Lūdimus post merīdiem."},
      {"On a cold afternoon the cat slept.", "Diē frīgidō post merīdiem fēlēs dormīvit."},
      {"I don't know where.", "Nesciō ubi sit."},
      {"She doesn't know where.", "Nescit ubi sit."},
      {"We must... be going now.", "Nunc īre dēbēmus."},
      {"You should not... be sitting there.", "Ibi sedēre nōn dēbēs."},
      {"Tomorrow we can swim in... in the lake.", "Crās in lacū nāre possumus."},
      {"What are you afraid of?", "Quid timēs?"},
      {"What is she afraid of?", "Quid timet?"},
      {"What would the old king say?", "Quid rēx vetus dīceret?"},
      {"The boy is late for school.", "Puer ad lūdum sērō venit."},
  });
  CHECK(run({"After this the king will need a new crown."})[0].text.rfind("Post hoc ", 0) == 0);   // C26: opus erit
}

TEST_CASE("rules-j: relative clauses: across song lines, where after a noun, can inside can (work item f)") {
  NEED_REAL();
  CHECK(run({"\xE2\x99\xAA I dream about a little boat", "\xE2\x99\xAA That carries me to school"})[1].text ==
        "\xE2\x99\xAA Quae mē ad lūdum portat");
  CHECK(run({"\xE2\x99\xAA I know a garden by the river", "\xE2\x99\xAA That nobody has seen"})[1].text ==
        "\xE2\x99\xAA Quod nēmō vīdit");
  expectEach({
      {"We walked to the old house where my grandmother lives.", "Ad domum veterem in quā avia mea habitat ambulāvimus."},
      {"We visited the town where my uncle works.", "Oppidum in quō avunculus meus labōrat vīsitāvimus."},
      {"I could find a book that I could read.", "Librum quem legere possem invenīre poteram."},
      {"She can sing a song that I can understand.", "Carmen quod intellegere possim canere potest."},
  });
}

TEST_CASE("rules-j: a sound word in quotes is kept as written (work item g)") {
  NEED_REAL();
  expectEach({
      {"The dog says \"woof\".", "Canis \"woof\" dīcit."},
      {"Cows say \"moo\" and dogs say \"woof\".", "Bovēs \"moo\" dīcunt et canēs \"woof\" dīcunt."},
      {"She said \"yes\".", "Dīxit ita."},   // a word with a Latin answer is translated
      {"Cats say it and dogs say it.", "Fēlēs id dīcunt et canēs id dīcunt."},
  });
}

TEST_CASE("rules-j: an invented word of a preposition and a known noun (work item h)") {
  NEED_REAL();
  {
    const Out x = run({"They flew overcloud."})[0];
    CHECK(x.text == "Super nūbem volāvērunt.");
    CHECK(hasFlag(x, "derived-word"));
    CHECK(x.conf == rules::Confidence::Check);
  }
  CHECK(run({"The children hid underbridge."})[0].text.find("sub ponte") != std::string::npos);
}

TEST_CASE("rules-j: fixes after the blind check (own sentences, C24)") {
  NEED_REAL();
  expectEach({
      {"The boy put on his hat and went out.", "Puer pilleum suum induit et exiit."},
      {"She put the book on the table.", "Librum in mēnsā posuit."},
      {"We must run, or the bus will leave.", "Currere dēbēmus; aliter lāophorīum exībit."},
      {"I think she likes him.", "Putō eum eī placēre."},
      {"I don't think she likes me.", "Nōn putō mē eī placēre."},
      {"The boy took his hat and kissed his mother.", "Puer pilleum suum sūmpsit et mātrem suam ōsculātus est."},
  });
  CHECK(run({"If dogs could talk and cats could sing, the farm would be loud."})[0].text.find("et fēlēs canere possent") !=
        std::string::npos);
  CHECK(run({"I like red."})[0].text == "Rubrum amō.");
}

TEST_CASE("rules-j: deterministic with the new rules, in both latinity modes") {
  NEED_REAL();
  const std::vector<std::string> src = {"Grandfather, are you tired?", "Yes, I am very tired.",
                                        "The sea would be what the sky is.", "The dog says \"woof\".",
                                        "\xE2\x99\xAA I dream about a little boat", "\xE2\x99\xAA That carries me to school"};
  for (rules::Latinity lt : {rules::Latinity::Wide, rules::Latinity::Classical}) {
    std::string first;
    for (int k = 0; k < 2; ++k) {
      std::unique_ptr<rules::Engine> e = engine();
      std::vector<rules::CueInput> in;
      for (size_t i = 0; i < src.size(); ++i) {
        rules::CueInput c;
        c.index = (uint32_t)i;
        c.sourceText = src[i];
        c.startMs = (int64_t)i * 4000;
        c.endMs = c.startMs + 3500;
        in.push_back(c);
      }
      rules::Options o;
      o.speakerGender = 'f';
      o.latinity = lt;
      auto r = e->translate(in, o, rules::Context{}, nullptr, nullptr);
      REQUIRE(r.ok());
      std::string all;
      for (const auto& c : r.value()) all += c.target + "\n";
      if (k == 0) first = all;
      else CHECK(all == first);
    }
    CHECK(first.find("fessus sum") != std::string::npos);
  }
}

// ================================================================================================================
// C26 (RULES-K, Latin loop 5 on the public-domain tuning sample). Generalisation guard: every rule below has at least
// two sentences of our own, written before the rule was run on the tuning sample (docs/rules_en_notes.md "Quality
// loop 5 (C26)").
TEST_CASE("rules-k: ride, alternative questions, must not, whose (work item d)") {
  NEED_REAL();
  expectEach({
      {"The knight rode to the castle.", "Mīles ad castrum equitāvit."},
      {"We rode through the forest.", "Per silvam equitāvimus."},
      {"The boy rode his horse to the river.", "Puer equō suō ad flūmen vectus est."},
      {"My sister rides a white horse.", "Soror mea equō albō vehitur."},
      {"Is this your cat or mine?", "Estne haec fēlēs tua an mea?"},
      {"Is the box big or small?", "Estne arca magna an parva?"},
      {"Are you a boy or a girl?", "Esne puer an puella?"},
      {"Is this your book or his?", "Estne hic liber tuus an eius?"},
      {"You must not open that door.", "Illam iānuam aperīre nōn dēbēs."},
      {"You must not touch the fire.", "Ignem tangere nōn dēbēs."},
      {"We must not wake the baby.", "Īnfantem excitāre nōn dēbēmus."},
      {"Whose book is this?", "Cuius est hic liber?"},
      {"Whose shoes are these?", "Cuius sunt hī calceī?"},
      {"Whose dog is barking?", "Cuius canis lātrat?"},
  });
  for (const char* s : {"You must not open that door.", "You must not touch the fire.", "Whose book is this?"})
    CHECK(run({s})[0].conf != rules::Confidence::Fix);
}

TEST_CASE("rules-k: tomorrow, give + person + something to drink, without + -ing, secret (work item d)") {
  NEED_REAL();
  expectEach({
      {"We will go to the market tomorrow.", "Crās ad forum ībimus."},
      {"I will come back tomorrow.", "Crās redībō."},
      {"Tomorrow the king will arrive.", "Crās rēx perveniet."},
      {"Give the children something to drink.", "Puerīs aliquid ad bibendum dā."},
      {"Bring the old man something to eat.", "Senī aliquid ad edendum fer."},
      {"They walked for hours without finding water.", "Hōrās ambulāvērunt neque aquam invēnērunt."},
      {"He left without saying goodbye.", "Exiit nec valedīxit."},
      {"The dog waited without barking.", "Canis mānsit nec lātrāvit."},
      {"I know a secret.", "Arcānum sciō."},
      {"She told me a secret.", "Arcānum mihi dīxit."},
  });
}

TEST_CASE("rules-k: fragments keep the case and agreement of their phrase (work item b)") {
  NEED_REAL();
  expectEach({
      {"A tall man in a green coat.", "Vir altus in palliō viridī."},
      {"An old woman in a red cloak.", "Anus in palliō rubrō."},
      {"The lion, the tiger and the bear.", "Leō et tigris et ursus."},
      {"The bread, the milk and the eggs.", "Pānis et lac et ōva."},
      {"Not my sister!", "Nōn soror mea!"},
      {"Not the old dog!", "Nōn canis vetus!"},
      {"Just an old box.", "Tantum arca vetus."},
      {"Only a little bird.", "Tantum avis parva."},
      {"A crown made of gold.", "Corōna quae ex aurō facta est."},
      {"A boat made of paper.", "Nāvis quae ē chartā facta est."},
      {"They waited for a long time.", "Diū mānsērunt."},
      {"We talked for a long time.", "Locūtae sumus diū."},
      {"There is a tree in the middle of the garden.", "In mediō hortī est arbor."},
      {"The boat was in the middle of the lake.", "Nāvis in mediō lacūs erat."},
  });
  // a cue that starts with a lower-case word continues the sentence of the cue before
  CHECK(run({"We found the cat.", "and the little dog."})[1].text == "Et canem parvum.");
  CHECK(run({"She called the boys.", "and the girls too."})[1].text == "Et puellās quoque.");
  for (const char* s : {"A tall man in a green coat.", "An old woman in a red cloak.", "Just an old box."})
    CHECK(run({s})[0].conf != rules::Confidence::Fix);
}

TEST_CASE("rules-k: rules from the tuning sample, each with own sentences (work item a)") {
  NEED_REAL();
  expectEach({
      // get + a thing that is given -> accipiō
      {"Tomorrow you will get a present.", "Crās dōnum accipiēs."},
      {"The good children will get a prize.", "Puerī bonī praemium accipient."},
      // a match that is lit is a fax
      {"She struck a match.", "Facem accendit."},
      {"The lighted match fell.", "Fax accēnsa cecidit."},
      // the rest of us / all of them
      {"She is taller than the rest of us.", "Altior est quam nōs cēterae."},
      {"The rest of the children laughed.", "Cēterī puerī rīsērunt."},
      // only + subject -> sōlus
      {"Only cats eat fish.", "Sōlae fēlēs piscem edunt."},
      {"Only the king knows the secret.", "Sōlus rēx arcānum scit."},
      // together after a comparison
      {"The bear is stronger than all of us together.", "Ursus validior est quam nōs omnēs ūnā."},
      // nor + any person -> nec quisquam
      {"Nor did I see any person on the road.", "Nec quemquam in viā vīdī."},
      // never + anyone -> nēminem umquam
      {"He never helps anyone.", "Nēminem umquam adiuvat."},
      {"I have never met anyone there.", "Nēminī umquam ibi occurrī."},
      // know + a person -> nōvī
      {"Do you know my uncle?", "Nōvistīne avunculum meum?"},
      {"All of you must come.", "Omnēs venīre dēbētis."},
      {"She knew the old king.", "Rēgem veterem nōverat."},
      // brains = wits, a lot of + a mass noun
      {"The boy wants a lot of brains.", "Puer multum cerebrī vult."},
      {"You have good brains.", "Cerebrum bonum habēs."},
      // let + object + verb -> sinō + accusative + infinitive
      {"The farmer never lets the goats eat the corn.", "Agricola numquam caprās sinit annōnam edere."},
      {"She let the cat come into the kitchen.", "Fēlem sīvit in culīnam venīre."},
      // need -> opus est + dative (order_la.txt need.opus), the thing in the ablative as the teacher's row
      {"We need a boat.", "Nāve nōbīs opus est."},
      {"The birds need water.", "Aquā avibus opus est."},
      {"I must have time to think it over.", "Tempore mihi opus est ut id cōgitem."},
      // an elliptical prohibition is nōlī alone; anything you want -> quidquid vīs; strike -> feriō
      {"Don't push me, please don't!", "Nōlī mē pellere, nōlī quaesō!"},
      {"I will do anything you want.", "Faciam quidquid vīs."},
      {"Take whatever you like.", "Sūme quidquid tibi placet."},
      {"Don't strike the dog.", "Nōlī canem ferīre."},
      // anyone / people in trouble -> eōs quī in perīculō sunt (phrasebook, at the end of the clause)
      {"We must help anyone in trouble.", "Adiuvāre dēbēmus eōs quī in perīculō sunt."},
      {"The doctor helps people in trouble.", "Medicus adiuvat eōs quī in perīculō sunt."},
      // a coordinated clause with its own subject after a predicate noun
      {"You are a good friend, for you have helped me.", "Amīcus bonus es nam mē adiūvistī."},
  });
}

TEST_CASE("rules-k: words of the Oz register are never bracketed (work item c)") {
  NEED_REAL();
  expectEach({
      {"The housetop was white.", "Apex domūs albus erat."},
      {"We climbed to the hilltop.", "Ad apicem collis scandimus."},
      {"The pink and orange sky.", "Caelum roseum et aurantium."},
      {"A small brown mouse.", "Mūs parvus fuscus."},
      {"The road of yellow brick.", "Via lateris flāvī."},
      {"He spoke with great kindness.", "Benignitāte magnā locūtus est."},
      {"The colourful birds sang.", "Avēs versicolōrēs cecinērunt."},
      {"The road was bumpy.", "Via aspera erat."},
      {"The woodman's joints were oiled.", "Artūs lignātōris ūnctī erant."},
      {"The emerald city sparkled.", "Urbs smaragdīna scintillāvit."},
      {"A beautiful silk dress.", "Vestis pulchra sēricī."},
      {"Gee, that is strange.", "Papae, id mīrum est."},
      {"Oh, bother!", "Ō, vah!"},
      {"The hatless man shivered.", "Vir quī pilleō caret horruit."},
      {"The roofless house was cold.", "Domus quae tēctō caret frīgida erat."},
  });
  for (const char* s : {"The housetop was white.", "The road of yellow brick.", "The hatless man shivered.",
                        "The colourful birds sang.", "A beautiful silk dress."}) {
    const Out o = run({s})[0];
    CHECK_MESSAGE(o.text.find('[') == std::string::npos, s << " -> " << o.text);
    CHECK(o.conf != rules::Confidence::Fix);
  }
}

TEST_CASE("rules-k: deterministic with the new rules, in both latinity modes") {
  NEED_REAL();
  const std::vector<std::string> src = {"The knight rode to the castle.", "Is this your cat or mine?",
                                        "Whose book is this?", "They walked for hours without finding water.",
                                        "We found the cat.", "and the little dog.", "We need a boat.",
                                        "The hatless man shivered.", "He never helps anyone."};
  for (rules::Latinity lt : {rules::Latinity::Wide, rules::Latinity::Classical}) {
    std::string first;
    for (int k = 0; k < 2; ++k) {
      std::unique_ptr<rules::Engine> e = engine();
      std::vector<rules::CueInput> in;
      for (size_t i = 0; i < src.size(); ++i) {
        rules::CueInput c;
        c.index = (uint32_t)i;
        c.sourceText = src[i];
        c.startMs = (int64_t)i * 4000;
        c.endMs = c.startMs + 3500;
        in.push_back(c);
      }
      rules::Options o;
      o.speakerGender = 'f';
      o.latinity = lt;
      auto r = e->translate(in, o, rules::Context{}, nullptr, nullptr);
      REQUIRE(r.ok());
      std::string all;
      for (const auto& c : r.value()) all += c.target + "\n";
      if (k == 0) first = all;
      else CHECK(all == first);
    }
    CHECK(first.find("equitāvit") != std::string::npos);
    CHECK(first.find("Cuius est hic liber?") != std::string::npos);
  }
}

TEST_CASE("rules-k: fixes after the blind check (own sentences, C26)") {
  NEED_REAL();
  expectEach({
      {"The careless boy broke the cup.", "Puer neglegēns calicem frēgit."},
      {"She is very careless.", "Valdē neglegēns est."},
      {"The smell of the fresh bread filled the kitchen.", "Odor pānis recentis culīnam implēvit."},
  });
  // a past verb hung on a noun of the subject phrase is the main verb; an adjective with an object is a verb
  CHECK(run({"The song of the little bird pleased everyone."})[0].text.find("placuit") != std::string::npos);
  CHECK(run({"The roar of the angry lion frightened the girls."})[0].text.find("terru") != std::string::npos);
}

// ================================================================================================================
// C28 (Latin loop 6): cue context. tests/regression/own_turns.en.srt (our own dialogue: speaker dashes, sentences over
// two or three cues, lower-case continuations, answer fragments, interjections, vocatives, song lines) vs its gold.

TEST_CASE("rules-l: end to end on own_turns.en.srt vs the gold Latin (report; determinism; cue identity)") {
  NEED_REAL();
  std::vector<rules::CueInput> in = regressionCues(1, "own_turns.en.srt");
  REQUIRE(in.size() == 120);
  rules::Options o;
  o.fidelity = 2;
  o.speakerGender = 'm';
  rules::Context ctx;
  auto r1 = engine()->translate(in, o, ctx, nullptr, nullptr);
  REQUIRE(r1.ok());
  auto r2 = engine()->translate(in, o, ctx, nullptr, nullptr);   // a fresh engine: byte-identical
  REQUIRE(r2.ok());
  REQUIRE(r1->size() == 120);
  REQUIRE(r2->size() == 120);
  bool same = true;
  for (size_t i = 0; i < 120; ++i)
    same = same && r1.value()[i].target == r2.value()[i].target && r1.value()[i].confidence == r2.value()[i].confidence;
  CHECK(same);
  std::ifstream g(repo() / "tests" / "regression" / "expected" / "own_turns.la.gold.txt");
  std::vector<std::string> gold;
  std::string line;
  while (std::getline(g, line))
    if (!line.empty() && line[0] != '#') gold.push_back(line);
  REQUIRE(gold.size() == 120);
  int matches = 0, exact = 0, wrongOk = 0;
  std::map<std::string, int> conf;
  std::ostringstream table;
  table << "| # | source | gold | ours | conf |\n|---|---|---|---|---|\n";
  for (size_t i = 0; i < 120; ++i) {
    const rules::CueOutput& c = r1.value()[i];
    CHECK_MESSAGE(!c.target.empty(), "empty target for cue " << i + 1);   // C22: no cue is ever emptied
    CHECK(c.index == in[i].index);
    ++conf[confName(c.confidence)];
    const std::string ours = flat(c.target);
    bool match = false, exactMatch = false;
    for (const std::string& alt : splitAlt(gold[i])) {
      match = match || norm(alt) == norm(ours);
      exactMatch = exactMatch || text::nfc(alt) == text::nfc(ours);
    }
    matches += match;
    exact += exactMatch;
    if (!match && c.confidence == rules::Confidence::Ok) ++wrongOk;
    if (!match) {
      std::string chk;
      for (const auto& k : c.checks)
        if (!k.ok) chk += k.id + " ";
      for (const auto& f : c.flags)
        if (f != "tags") chk += f + " ";
      std::string g2 = gold[i];
      for (size_t at; (at = g2.find(" | ")) != std::string::npos;) g2.replace(at, 3, " / ");
      table << "| " << i + 1 << " | " << in[i].sourceText << " | " << g2 << " | " << ours << " | "
            << confName(c.confidence) << (chk.empty() ? "" : " " + chk) << "|\n";
    }
  }
  std::ostringstream rep;
  rep << "Regression own_turns.en.srt -> Latin, fidelity 2, speaker m\n";
  rep << "match rate (normalised, any gold alternative): " << matches << " / 120\n";
  rep << "exact (macrons and punctuation too): " << exact << " / 120\n";
  rep << "confidence: ok " << conf["ok"] << ", check " << conf["check"] << ", fix " << conf["fix"] << "\n";
  rep << "wrong among OK: " << wrongOk << "\n\nMismatches:\n" << table.str() << "\nAll outputs:\n";
  for (size_t i = 0; i < 120; ++i) {
    const rules::CueOutput& c = r1.value()[i];
    std::string fl;
    for (const auto& f : c.flags) fl += f + " ";
    rep << i + 1 << "\t" << in[i].sourceText << "\t" << flat(c.target) << "\t" << confName(c.confidence) << "\t" << fl
        << "\n";
  }
  std::ofstream(buildDir() / "regression_report_turns.txt") << rep.str();
  MESSAGE("own_turns regression: " << matches << " / 120 match the gold; confidence ok " << conf["ok"] << " / check "
                                   << conf["check"] << " / fix " << conf["fix"] << "; wrong among OK " << wrongOk);
  CHECK(wrongOk == 0);
  // C28: first run before any rule 48 / 120; after the rules 100 / 120 without the 19 alternatives proposed to the main
  // agent (119 / 120 with them; docs/rules_en_notes.md "Quality loop 6 (C28)")
  CHECK(matches >= 100);
}

// C28 rules, each with own sentences (not from own_turns.en.srt).
TEST_CASE("rules-l: a sentence over several cues is re-split by clause at the same cue boundary (work item a)") {
  NEED_REAL();
  auto pieces = [](const std::vector<std::string>& src, const std::vector<std::string>& want, char g = 'm') {
    const std::vector<Out> o = run(src, g);
    REQUIRE(o.size() == want.size());
    for (size_t i = 0; i < want.size(); ++i) {
      CHECK_MESSAGE(o[i].text == want[i], src[i] << " -> " << o[i].text << " (expected " << want[i] << ")");
      CHECK(!o[i].text.empty());   // C22: no cue is ever emptied
    }
  };
  pieces({"The old man sat", "under the big tree."}, {"Senex sedēbat", "sub arbore magnā."});
  pieces({"We will wait for you", "at the gate."}, {"Tē exspectābimus", "in portā."});
  // a relative word goes with its own clause, a copula left at the end of a cue with its predicate
  pieces({"This is the house that", "my father built."}, {"Haec est domus", "quam pater meus aedificāvit."});
  pieces({"This is the man who", "saved my dog."}, {"Hic est vir", "quī canem meum servāvit."});
  pieces({"My sister is", "very tired."}, {"Soror mea", "valdē fessa est."});
  // a preposition left at the end of a cue goes with its noun; a perfect stays whole
  pieces({"He stood in front of", "the door."}, {"Stābat", "ante iānuam."});
  CHECK(run({"When the sun rose,", "the birds sang", "in the trees."})[0].text == "Cum sōl ortus est,");
  pieces({"The girls played", "in the garden", "until the evening."}, {"Puellae lūsērunt", "in hortō", "ad vesperum."});
}

TEST_CASE("rules-l: an answer takes the case its question gives it (work item b)") {
  NEED_REAL();
  auto answer = [](const char* q, const char* a, const char* want) {
    const std::vector<Out> o = run({q, a});
    CHECK_MESSAGE(o[1].text == want, q << " / " << a << " -> " << o[1].text << " (expected " << want << ")");
  };
  answer("Whom do you love?", "My mother.", "Mātrem meam.");
  answer("What did you eat?", "Bread and cheese.", "Pānem et cāseum.");
  answer("To whom did you give the book?", "To my brother.", "Frātrī meō.");
  answer("To whom did she send the letter?", "To her aunt.", "Amitae suae.");
  answer("Whose horse is this?", "The king's.", "Rēgis.");
  answer("Where are you going?", "Home.", "Domum.");
  answer("How many apples do you want?", "Three.", "Tria.");
  answer("What are you reading?", "A letter from my father.", "Epistulam ā patre meō.");
  // a subject question keeps the nominative, a place answer its preposition
  answer("Who is there?", "The queen.", "Rēgīna.");
  answer("Where did you find the cat?", "In the kitchen.", "In culīnā.");
  // the answer of a short follow-up question ("And then?") still answers the question before it
  const std::vector<Out> f = run({"Where are you going?", "To the river.", "And then?", "Home."});
  CHECK(f[3].text == "Domum.");
  // the fragment says how it was attached; with no question before it nothing is guessed
  CHECK(hasFlag(run({"Whom do you love?", "My mother."})[1], "answer-case"));
  CHECK(run({"My mother."})[0].text == "Māter mea.");
}

TEST_CASE("rules-l: speaker turns: dashes, people addressed, replies (work item c)") {
  NEED_REAL();
  CHECK(run({"- Come here! - I am coming!"})[0].text == "- Venī hūc! - Veniō!");
  CHECK(run({"- I am tired. - So am I."}, 'm')[0].text == "- Fessus sum. - Et ego.");
  // a group addressed makes the orders, greetings and "you" of its sentence plural
  CHECK(run({"Boys, sit down."})[0].text == "Puerī, sedēte.");
  CHECK(run({"Hello, girls!"})[0].text == "Salvēte, puellae!");
  // a name or a word of address alone is called out (vocative); after a "who" question it is the answer
  CHECK(run({"Peter!"})[0].text == "Petre!");
  CHECK(run({"Mother!"})[0].text == "Māter!");
  CHECK(run({"Who did it?", "Peter!"})[1].text == "Petrus!");
  // a question with the person addressed last; the person addressed before a question
  CHECK(run({"Is he your brother, Julia?"})[0].text == "Estne frāter tuus, Iūlia?");
  CHECK(run({"Children, who broke the window?"})[0].text == "Puerī, quis fenestram frēgit?");
  // the reply's speaker is the person asked
  const std::vector<Out> r = run({"Anna, are you tired?", "- Yes, very tired."}, 'm');
  CHECK(r[0].text == "Anna, esne fessa?");
  CHECK(r[1].text == "- Ita, valdē fessa.");
  // a speaker dash ends the continuation: a lower-case turn after a dash is not glued to the line before
  const std::vector<Out> d = run({"I saw the queen", "- and the king?"});
  CHECK(d[0].text.find("Rēgīnam") != std::string::npos);
  CHECK(d[1].text == "- Et rēx?");   // a new turn: no continuation case
  CHECK(!d[1].text.empty());
}

TEST_CASE("rules-l: a cue of one or two words is OK only from the phrasebook or the names table (work item d)") {
  NEED_REAL();
  for (const char* s : {"Yes.", "Thank you.", "Peter!", "Why not?"}) {
    const Out o = run({s})[0];
    CHECK_MESSAGE(o.conf == rules::Confidence::Ok, s << " -> " << o.text);
  }
  for (const char* s : {"The queen.", "The garden.", "Mother!"}) {
    const Out o = run({s})[0];
    CHECK_MESSAGE(o.conf != rules::Confidence::Ok, s << " -> " << o.text);
    CHECK(hasFlag(o, "short-cue"));
  }
  // a piece of a sentence over two cues is attached: no short-cue flag
  CHECK(!hasFlag(run({"He stood in front of", "the door."})[1], "short-cue"));
}

TEST_CASE("rules-l: agreement inside fragments, as in a clause (work item e)") {
  NEED_REAL();
  // a state said alone speaks of the speaker (the reply's speaker when the cue before addressed someone)
  CHECK(run({"Anna, go home.", "Alone?"}, 'm')[1].text == "Sōla?");
  CHECK(run({"Tired?"}, 'm')[0].text == "Fessus?");
  CHECK(run({"Tired?"}, 'f')[0].text == "Fessa?");
  CHECK(run({"Tired?"}, 'm')[0].conf != rules::Confidence::Ok);
  // a number alone agrees with what it counts; "a little" + a mass noun is a partitive
  CHECK(run({"How many loaves do you want?", "Two."})[1].text == "Duōs.");
  CHECK(run({"Give me a little water."})[0].text == "Dā mihi paulum aquae.");
  CHECK(run({"A little wine, please."})[0].text == "Paulum vīnī, quaesō.");
  // a possessor alone is a genitive; a pronoun alone stays
  CHECK(run({"The baker's."})[0].text == "Pānificis.");
  CHECK(run({"Whose dog is this?", "My father's."})[1].text == "Patris meī.");
  CHECK(run({"And you, Peter?"})[0].text == "Et tū, Petre?");
  // "It's me, your brother.": the pronoun is the subject, the noun phrase an apposition
  CHECK(run({"It's me, your brother."}, 'm')[0].text == "Ego sum, frāter tuus.");
  CHECK(run({"It's us!"})[0].text == "Nōs sumus!");
}

TEST_CASE("rules-l: words and phrases of dialogue (own sentences, C28)") {
  NEED_REAL();
  expectEach({
      {"We haven't eaten yet.", "Nōndum ēdimus."},
      {"She is not here yet.", "Hīc nōndum est."},
      {"Come back tonight.", "Redī hāc nocte."},
      {"He arrived last week.", "Superiōre hebdomade pervēnit."},
      {"I would like a cup of water.", "Calicem aquae velim."},
      {"Got her!", "Eam cēpī!"},
      {"That way, children!", "Illūc, puerī!"},
      {"Here, drink this.", "Ecce, bibe hoc."},
      {"They live by the sea.", "Prope mare habitant."},
      {"We live in a small house.", "In domō parvā habitāmus."},
      {"We walked past the school.", "Praeter lūdum ambulāvimus."},
      {"He stood in front of the door.", "Ante iānuam stābat."},
      {"There!", "Ibi!"},
      {"Did you see that?", "Vīdistīne illud?"},
      {"Give the book back to your teacher.", "Redde magistrō tuō librum."},
      {"She came from Rome.", "Rōmā vēnit."},
      {"The window was open.", "Fenestra aperta erat."},
      {"The soup is ready.", "Iūs parātum est."},
      {"The crowd ran away.", "Turba aufūgit."},
      {"This is the man who saved my dog.", "Hic est vir quī canem meum servāvit."},
  });
  // ", or" after an order: aliter; "until" opening a cue: dōnec + subjunctive
  CHECK(run({"Be quiet,", "or you will wake the baby."})[1].text == "Aliter īnfantem excitābis.");
  CHECK(run({"Hurry,", "or we will be late."})[1].text.find("Aliter") == 0);
  CHECK(run({"♪ Sing, my child, sing ♪"}, 'm')[0].text == "♪ Cane, mī fīlī, cane ♪");
  // "it" after a person or an animal points further back: a guess (Check)
  CHECK(hasFlag(run({"A story about a horse.", "The horse could fly.", "I finished it."})[2], "antecedent-guess"));
}

TEST_CASE("rules-l: deterministic with the new rules, in both latinity modes") {
  NEED_REAL();
  const std::vector<std::string> src = {"Marcus, where are you going?", "To the market.", "And then?", "Home.",
                                        "This is the boy who", "found our cat.", "- Are you tired? - Very tired.",
                                        "Boys, sit down.", "Whose bag is this?", "The teacher's.", "Two, and a little milk."};
  for (rules::Latinity lt : {rules::Latinity::Wide, rules::Latinity::Classical}) {
    std::string first;
    for (int k = 0; k < 2; ++k) {
      std::unique_ptr<rules::Engine> e = engine();
      std::vector<rules::CueInput> in;
      for (size_t i = 0; i < src.size(); ++i) {
        rules::CueInput c;
        c.index = (uint32_t)i;
        c.sourceText = src[i];
        c.startMs = (int64_t)i * 4000;
        c.endMs = c.startMs + 3500;
        in.push_back(c);
      }
      rules::Options o;
      o.latinity = lt;
      auto r = e->translate(in, o, rules::Context{}, nullptr, nullptr);
      REQUIRE(r.ok());
      REQUIRE(r->size() == src.size());
      std::string all;
      for (const auto& c : r.value()) {
        CHECK(!c.target.empty());
        all += c.target + "\n";
      }
      if (k == 0) first = all;
      else CHECK(all == first);
    }
    CHECK(first.find("Domum.") != std::string::npos);
    CHECK(first.find("quī fēlem nostram invēnit.") != std::string::npos);
  }
}

TEST_CASE("rules-l: fixes after the blind check (own sentences, C28)") {
  NEED_REAL();
  // "the one" + a place: a relative clause
  CHECK(run({"Which cup?", "The one on the table."})[1].text == "Is quī in mēnsā est.");
  CHECK(run({"Which girl?", "The one in the garden."})[1].text == "Ea quae in hortō est.");
  // promise / swear: accusative and future infinitive
  CHECK(run({"She promised to come."})[0].text == "Sē ventūram esse prōmīsit.");
  CHECK(run({"We promised to help him."}, 'm')[0].text == "Eum nōs adiūtūrōs esse prōmīsimus.");
  // "a lot" alone; "a lot of" stays the partitive
  CHECK(run({"Is it raining?", "- Yes, a lot."})[1].text == "- Ita, multum.");
  CHECK(run({"A lot of children came."})[0].text == "Multī puerī vēnērunt.");
  CHECK(run({"Do you like the garden?", "Yes, very much."})[1].text == "Ita, valdē.");
  // the reply to a question asked as "we" speaks to them all
  CHECK(run({"Mother, can we go out?", "- After dinner,", "if you are good."})[2].text == "Sī bonī estis.");
  CHECK(run({"Can we play now?", "Yes, if you are quiet."})[1].text == "Ita, sī tranquillī estis.");
}

// ================================================================================================================
// C30 (Latin loop 7): narrative prose cut mid-sentence. tests/regression/own_story.en.srt (our own short story in
// narrative English, cut at 35-42 characters mid-phrase, mid-clause and across sentence ends) vs its gold.

TEST_CASE("rules-m: end to end on own_story.en.srt vs the gold Latin (report; determinism; cue identity)") {
  NEED_REAL();
  std::vector<rules::CueInput> in = regressionCues(1, "own_story.en.srt");
  REQUIRE(in.size() == 120);
  rules::Options o;
  o.fidelity = 2;
  o.speakerGender = 'm';
  rules::Context ctx;
  auto r1 = engine()->translate(in, o, ctx, nullptr, nullptr);
  REQUIRE(r1.ok());
  auto r2 = engine()->translate(in, o, ctx, nullptr, nullptr);   // byte-identical on a second run
  REQUIRE(r2.ok());
  REQUIRE(r1->size() == 120);
  REQUIRE(r2->size() == 120);
  bool same = true;
  for (size_t i = 0; i < 120; ++i)
    same = same && r1.value()[i].target == r2.value()[i].target && r1.value()[i].confidence == r2.value()[i].confidence;
  CHECK(same);
  std::ifstream g(repo() / "tests" / "regression" / "expected" / "own_story.la.gold.txt");
  std::vector<std::string> gold;
  std::string line;
  while (std::getline(g, line))
    if (!line.empty() && line[0] != '#') gold.push_back(line);
  REQUIRE(gold.size() == 120);
  int matches = 0, exact = 0, wrongOk = 0;
  std::map<std::string, int> conf;
  std::ostringstream table;
  table << "| # | source | gold | ours | conf |\n|---|---|---|---|---|\n";
  for (size_t i = 0; i < 120; ++i) {
    const rules::CueOutput& c = r1.value()[i];
    CHECK_MESSAGE(!c.target.empty(), "empty target for cue " << i + 1);   // C22: no cue is ever emptied
    CHECK(c.index == in[i].index);
    ++conf[confName(c.confidence)];
    const std::string ours = flat(c.target);
    bool match = false, exactMatch = false;
    for (const std::string& alt : splitAlt(gold[i])) {
      match = match || norm(alt) == norm(ours);
      exactMatch = exactMatch || text::nfc(alt) == text::nfc(ours);
    }
    matches += match;
    exact += exactMatch;
    if (!match && c.confidence == rules::Confidence::Ok) ++wrongOk;
    if (!match) {
      std::string chk;
      for (const auto& k : c.checks)
        if (!k.ok) chk += k.id + " ";
      for (const auto& f : c.flags)
        if (f != "tags") chk += f + " ";
      table << "| " << i + 1 << " | " << in[i].sourceText << " | " << splitAlt(gold[i])[0] << " | " << ours << " | "
            << confName(c.confidence) << (chk.empty() ? "" : " " + chk) << "|\n";
    }
  }
  std::ostringstream rep;
  rep << "Regression own_story.en.srt -> Latin, fidelity 2, speaker m\n";
  rep << "match rate (normalised, any gold alternative): " << matches << " / 120\n";
  rep << "exact (macrons and punctuation too): " << exact << " / 120\n";
  rep << "confidence: ok " << conf["ok"] << ", check " << conf["check"] << ", fix " << conf["fix"] << "\n";
  rep << "wrong among OK: " << wrongOk << "\n\nMismatches:\n" << table.str() << "\nAll outputs:\n";
  for (size_t i = 0; i < 120; ++i) {
    const rules::CueOutput& c = r1.value()[i];
    std::string fl;
    for (const auto& f : c.flags) fl += f + " ";
    rep << i + 1 << "\t" << in[i].sourceText << "\t" << flat(c.target) << "\t" << confName(c.confidence) << "\t" << fl
        << "\n";
  }
  std::ofstream(buildDir() / "regression_report_story.txt") << rep.str();
  MESSAGE("own_story regression: " << matches << " / 120 match the gold; confidence ok " << conf["ok"] << " / check "
                                   << conf["check"] << " / fix " << conf["fix"] << "; wrong among OK " << wrongOk);
  CHECK(wrongOk == 0);
  CHECK(matches >= 0);
}
