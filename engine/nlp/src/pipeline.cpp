#include "vp/nlp.h"

namespace vp::nlp {

Pipeline::Pipeline(Lang lang, Tagger tagger, Parser parser)
    : lang_(lang), tokenizer_(lang), tagger_(std::move(tagger)), parser_(std::move(parser)) {}

Result<Pipeline> Pipeline::open(Lang lang, const std::string& tagPath, const std::string& depPath) {
  auto t = Tagger::open(tagPath);
  if (!t.ok()) return t.error();
  auto p = Parser::open(depPath);
  if (!p.ok()) return p.error();
  return Pipeline(lang, std::move(t.value()), std::move(p.value()));
}

void Pipeline::analyse(std::vector<Token>& tokens) const {
  tagger_.tag(tokens);
  parser_.parse(tokens);
  for (Token& t : tokens) {
    if (t.lower.empty()) t.lower = normalise(t.text);
    std::string lemma;
    if (lemmatizer_) {
      try {
        lemma = lemmatizer_(t.lower, t.upos);
      } catch (...) {
        lemma.clear();   // a throwing hook falls back to the rules; nothing escapes the module
      }
    }
    t.lemma = lemma.empty() ? ruleLemma(lang_, t) : std::move(lemma);
  }
}

std::vector<Token> Pipeline::analyse(std::string_view sentence) const {
  std::vector<Token> tokens = tokenizer_.tokenize(sentence);
  analyse(tokens);
  return tokens;
}

}  // namespace vp::nlp
