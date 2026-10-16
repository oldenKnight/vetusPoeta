// `vpengine inspect` and `vpengine check`: developer and support tools on stdout.
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <set>

#include "commands.h"
#include "views.h"
#include "vp/fs.h"
#include "vp/lex.h"
#include "vp/subs.h"

namespace vpcli {

namespace {
std::string featureLine(const vp::rules::Features& f) {
  std::string out;
  for (const std::string* s : {&f.pos, &f.case_, &f.number, &f.gender, &f.person, &f.tense, &f.mood, &f.voice, &f.degree})
    if (!s->empty()) out += (out.empty() ? "" : " ") + *s;
  return out;
}

std::string join(const std::vector<std::string>& v) {
  std::string out;
  for (const std::string& s : v) out += (out.empty() ? "" : ",") + s;
  return out;
}
}  // namespace

int cmdInspect(const Args& args) {
  try {
    if (args.size() != 2) {
      std::fprintf(stderr, "usage: vpengine inspect <file.vpl> <word>\n");
      return 2;
    }
    vp::Result<vp::lex::Lexicon> r = vp::lex::Lexicon::open(vp::fs::u8path(args[0]));
    if (!r) {
      std::fprintf(stderr, "%s: %s\n", vp::errorCodeName(r.error().code), r.error().message.c_str());
      return 1;
    }
    const vp::lex::Lexicon& lx = r.value();
    const vp::lex::Stats st = lx.stats();
    const bool greek = looksGreek(args[1]);
    const std::string key = lexKey(args[1], greek);
    std::printf("lexicon %s v%u.%u: %u keys, %u analyses, %u lemmas\n", std::string(lx.lang()).c_str(), st.major,
                st.minor, st.keys, st.analyses, st.lemmas);
    std::printf("word '%s' key '%s' (%s)\n", args[1].c_str(), key.c_str(), greek ? "greek_key" : "latin_key");
    std::vector<vp::lex::Analysis> an;
    if (!lx.lookup(key, an) || an.empty()) {
      std::vector<std::string_view> near;
      lx.prefix(key.substr(0, key.size() > 2 ? key.size() - 1 : key.size()), 8, near);
      std::printf("no analyses\n");
      for (std::string_view k : near) std::printf("  near: %.*s\n", static_cast<int>(k.size()), k.data());
      return 1;
    }
    std::set<uint32_t> lemmas;
    std::printf("%zu analyses:\n", an.size());
    for (const vp::lex::Analysis& a : an) {
      const vp::lex::Lemma l = lx.lemma(a.lemma);
      std::printf("  %.*s  <- %.*s [%u] %s  \"%.*s\"  %s\n", static_cast<int>(a.display.size()), a.display.data(),
                  static_cast<int>(l.head.size()), l.head.data(), a.lemma,
                  featureLine(featuresFromPacked(lx.feature(a.feat))).c_str(), static_cast<int>(l.glossEn.size()),
                  l.glossEn.data(), join(analFlagNames(a.flags)).c_str());
      lemmas.insert(a.lemma);
    }
    for (uint32_t id : lemmas) {
      const vp::lex::Lemma l = lx.lemma(id);
      std::vector<std::pair<uint32_t, std::string_view>> cells;
      lx.cells(id, cells);
      std::printf("lemma %u %.*s (%s%s%s, tier %u, flags %s) %.*s / %.*s: %zu cells\n", id,
                  static_cast<int>(l.head.size()), l.head.data(), posName(l.pos), *genderName(l.gender) ? " " : "",
                  genderName(l.gender), l.tier, join(lemmaFlagNames(l.flags)).c_str(),
                  static_cast<int>(l.glossEn.size()), l.glossEn.data(), static_cast<int>(l.glossEs.size()),
                  l.glossEs.data(), cells.size());
      for (const auto& c : cells) {
        std::string extra = join(extraNames(vp::feat::unpack(c.first).extra));
        std::printf("  %-48s %.*s%s%s\n", featureLine(featuresFromPacked(c.first)).c_str(),
                    static_cast<int>(c.second.size()), c.second.data(), extra.empty() ? "" : "  ", extra.c_str());
      }
    }
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "inspect failed: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "inspect failed\n");
  }
  return 1;
}

int cmdCheck(const Args& args) {
  try {
    std::string path;
    double limit = 17;
    for (size_t i = 0; i < args.size(); ++i) {
      if (args[i] == "--cps" && i + 1 < args.size()) limit = std::atof(args[++i].c_str());
      else if (path.empty()) path = args[i];
      else path.clear(), i = args.size();
    }
    if (path.empty() || !(limit > 0)) {
      std::fprintf(stderr, "usage: vpengine check <file.srt|.vtt|.ass|.txt> [--cps N]\n");
      return 2;
    }
    vp::subs::Format f;
    if (!formatFromPath(path, f)) {
      std::fprintf(stderr, "unsupported_format: %s\n", path.c_str());
      return 1;
    }
    vp::Result<std::string> bytes = vp::fs::readFile(path, 256ull << 20);
    if (!bytes) {
      std::fprintf(stderr, "%s: %s\n", vp::errorCodeName(bytes.error().code), bytes.error().message.c_str());
      return 1;
    }
    std::vector<uint8_t> b(bytes->begin(), bytes->end());
    vp::Result<vp::subs::Document> d = vp::subs::parse(b, f);
    if (!d) {
      std::fprintf(stderr, "%s: %s\n", vp::errorCodeName(d.error().code), d.error().message.c_str());
      return 1;
    }
    std::printf("file      %s\nformat    %s\nencoding  %s%s\nnewline   %s\ncues      %zu\nwarnings  %zu\n",
                path.c_str(), formatName(d->format), d->encoding.c_str(), d->bom ? " (BOM)" : "",
                d->newline == "\r\n" ? "CRLF" : "LF", d->cues.size(), d->warnings.size());
    for (const vp::subs::Warning& w : d->warnings)
      std::printf("  cue %u: %s %s\n", w.index, w.kind.c_str(), w.detail.c_str());
    size_t fast = 0;
    for (const vp::subs::Cue& c : d->cues) {
      const double cps = vp::subs::charsPerSecond(c);
      if (cps > limit) {
        if (fast++ == 0) std::printf("cps over %.1f:\n", limit);
        std::printf("  cue %u: %.1f cps  %s\n", c.index, cps, c.plainText().c_str());
      }
    }
    std::printf("fast cues %zu\n", fast);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "check failed: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "check failed\n");
  }
  return 1;
}

}  // namespace vpcli
