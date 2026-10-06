// EN/ES source analysis: tokeniser, perceptron tagger (UPOS + coarse features), greedy arc-eager parser,
// lemmatiser hook. Models are .vpt files (DESIGN.md section 17) trained by tools/train; the C++ decoders build the
// same feature strings (FNV-1a 64) and the same int16 sums as the Python trainers and break ties by lowest label
// index, so they reproduce the Python outputs exactly (tests/fixtures/nlp/*.golden.tsv). Nothing here throws.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "vp/result.h"

namespace vp::nlp {

enum class Lang { En, Es };

struct Token {
  std::string text, lower;   // surface form; lower = Unicode lower case with curly quotes made straight
  int start = 0, end = 0;    // byte offsets of text in the tokenised string ([start, end))
  std::string upos;          // UD UPOS tag ("NOUN", ...), empty before tagging
  std::string lemma;         // set by Pipeline (lemmatiser hook or the built-in rules)
  std::string deprel;        // universal deprel without subtype ("nsubj", "root", ...), empty before parsing
  int head = -1;             // 1-based index of the head token in the sentence, 0 = root, -1 = not parsed
  uint32_t feats = 0;        // packed morphological features, see namespace morph below
};

// Packed Token::feats. Each group is a small unsigned field; 0 = absent.
namespace morph {
enum Shift : uint32_t { NumberShift = 0, PersonShift = 2, TenseShift = 4, VerbFormShift = 7, MoodShift = 10,
                        PronTypeShift = 13 };
enum Number : uint32_t { NumSing = 1, NumPlur = 2, NumDual = 3 };                              // 2 bits
enum Person : uint32_t { Pers1 = 1, Pers2 = 2, Pers3 = 3 };                                     // 2 bits
enum Tense : uint32_t { TensePres = 1, TensePast = 2, TenseFut = 3, TenseImp = 4, TensePqp = 5 };  // 3 bits
enum VerbForm : uint32_t { VfFin = 1, VfInf = 2, VfPart = 3, VfGer = 4, VfConv = 5 };          // 3 bits
enum Mood : uint32_t { MoodInd = 1, MoodSub = 2, MoodImp = 3, MoodCnd = 4 };                    // 3 bits
enum PronType : uint32_t { PtPrs = 1, PtArt = 2, PtDem = 3, PtInd = 4, PtInt = 5, PtRel = 6, PtNeg = 7, PtTot = 8,
                           PtEmp = 9, PtRcp = 10, PtExc = 11, PtIntRel = 12 };                  // 4 bits
inline uint32_t get(uint32_t feats, Shift s) {
  const uint32_t width = (s == NumberShift || s == PersonShift) ? 3u : (s == PronTypeShift ? 15u : 7u);
  return (feats >> s) & width;
}
// UD spelling, groups in the order Number, Person, Tense, VerbForm, Mood, PronType: "Number=Sing|Person=3"; "_" if 0.
std::string toString(uint32_t feats);
// Inverse of toString for "Group=Value" pairs separated by '|'; unknown groups and values are ignored.
uint32_t fromString(std::string_view ud);
}  // namespace morph

// Lower case + curly quotes to straight (the tagger's and parser's word normalisation; Python features.norm).
std::string normalise(std::string_view word);

class Tokenizer {
 public:
  explicit Tokenizer(Lang lang = Lang::En) : lang_(lang) {}
  // Splits one sentence or line into tokens (text, lower, start, end set). Punctuation is split off; contractions
  // (don't, I'm, John's, 'em, goin') stay one token for the contraction table; hyphenated words, numbers with
  // separators (1,000.50  3:30  10/12), abbreviations (Mr.  U.S.  Sra.), URLs, e-mails, "..." stay whole; curly
  // quotes, ellipsis, dashes, ¿ ¡ « » are tokens of their own.
  std::vector<Token> tokenize(std::string_view text) const;
  void tokenize(std::string_view text, std::vector<Token>& out) const;
  Lang lang() const { return lang_; }

 private:
  Lang lang_;
};

struct Model;   // internal: one memory-mapped, validated .vpt file

// Errors: io (with hint) if the file cannot be read; internal with hint "model file damaged" for a bad magic,
// version, kind, size, layout or SHA-256.
class Tagger {
 public:
  Tagger();
  ~Tagger();
  Tagger(Tagger&&) noexcept;
  Tagger& operator=(Tagger&&) noexcept;
  static Result<Tagger> open(const std::string& path);
  bool isOpen() const { return model_ != nullptr; }
  std::string_view lang() const;   // "en", "es"
  std::string_view note() const;   // licence / attribution text (NOTE section)
  // Fills lower, upos and feats of every token from text (greedy, left to right). No-op if not open.
  void tag(std::vector<Token>& sentence) const;

 private:
  std::unique_ptr<Model> model_;
};

class Parser {
 public:
  Parser();
  ~Parser();
  Parser(Parser&&) noexcept;
  Parser& operator=(Parser&&) noexcept;
  static Result<Parser> open(const std::string& path);
  bool isOpen() const { return model_ != nullptr; }
  std::string_view lang() const;
  std::string_view note() const;
  // Fills head and deprel from lower and upos (call Tagger::tag first). No-op if not open.
  void parse(std::vector<Token>& sentence) const;

 private:
  std::unique_ptr<Model> model_;
};

// (lower, upos) -> lemma; return an empty string to fall back to the built-in rules.
using Lemmatizer = std::function<std::string(const std::string& lower, const std::string& upos)>;

// Built-in fallback: English regular inflection (-s, -es, -ies, -ed, -ied, -ing, -er, -est, a few irregular
// verbs); PROPN keeps its surface form; Spanish: the lower-cased form (no-op).
std::string ruleLemma(Lang lang, const Token& token);

class Pipeline {
 public:
  Pipeline() = default;
  Pipeline(Lang lang, Tagger tagger, Parser parser);
  static Result<Pipeline> open(Lang lang, const std::string& tagPath, const std::string& depPath);
  void setLemmatizer(Lemmatizer fn) { lemmatizer_ = std::move(fn); }
  // tokenise + tag + parse + lemmatise one sentence.
  std::vector<Token> analyse(std::string_view sentence) const;
  // tag + parse + lemmatise already tokenised words (text set).
  void analyse(std::vector<Token>& tokens) const;
  const Tokenizer& tokenizer() const { return tokenizer_; }
  const Tagger& tagger() const { return tagger_; }
  const Parser& parser() const { return parser_; }

 private:
  Lang lang_ = Lang::En;
  Tokenizer tokenizer_;
  Tagger tagger_;
  Parser parser_;
  Lemmatizer lemmatizer_;
};

// FNV-1a 64 of a feature string (0 remapped to 1), as in tools/train/features.py.
uint64_t featureHash(std::string_view feature);

}  // namespace vp::nlp
