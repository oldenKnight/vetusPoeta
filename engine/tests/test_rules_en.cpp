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
  CHECK(matches >= 68);   // C15 50 / 100; C17 68 / 100 without the gold alternatives it proposes (docs/rules_en_notes.md)
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
      {"He is more powerful than his brother.", "Fortior est quam frāter suus."},
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
      {"Only witches wear black hats.", "Modo sāgae capellōs nigrōs portant."},
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
  for (const char* s : {"The snowman is very big.", "The tinsmith mended the old pot.", "The milkmaid sang a song.",
                        "Two snowmen stood in the garden."}) {
    const std::vector<Out> o = run({s});
    CHECK_MESSAGE(o[0].text.find('[') == std::string::npos, s << " -> " << o[0].text);
    CHECK_MESSAGE(o[0].conf != rules::Confidence::Fix, s << " -> " << o[0].text);
  }
  CHECK(run({"The snowman is very big."})[0].text.find("nivis") != std::string::npos);
  CHECK(run({"The tinsmith mended the old pot."})[0].text.find("stannī") != std::string::npos);
  // numerals written in words: every one is rendered (a dropped numeral was wrong and OK)
  expectEach({
      {"I have forty sheep.", "Quadrāgintā ovēs habeō."},
      {"Fifteen birds sang.", "Quīndecim avēs cecinērunt."},
      {"He has twenty-two cows.", "Vīgintī duās vaccās habet."},
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
      {"He'd seen the wolf before.", "Lupum anteā vīderat."},
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
      {"'Tis a fine day.", "Diēs bellus est."},
      {"'Twas a cold night.", "Nox frīgida erat."},
      {"Gimme the ball!", "Dā mihi pilam!"},
      {"Gimme some bread.", "Dā mihi pānem."},
      {"Lemme see.", "Sine mē vidēre."},
      {"Lemme help you.", "Sine mē tē adiuvāre."},
  });
  // interjections
  expectEach({
      {"Hurrah, we won!", "Iō, vīcimus!"},
      {"Hurrah! The snow has come.", "Iō! Nix vēnit."},
      {"Hooray, the holidays!", "Iō, fēriae!"},
      {"Bravo, you did it!", "Euge, id fēcistī!"},
      {"Bravo, little brother!", "Euge, frāter parve!"},
  });
  // proper adjectives are adjectives, never dropped
  expectEach({
      {"He is a Roman soldier.", "Mīles Rōmānus est."},
      {"The Greek ship sailed away.", "Nāvis Graeca ēnāvigāvit."},
      {"I like Roman roads.", "Viās Rōmānās amō."},
  });
}
