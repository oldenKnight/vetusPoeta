// Every text the local model is given (DESIGN §11: two templates, versioned). Changing a template changes the
// model's answers: bump kPromptVersion and re-run the gate (tools/eval/llm_gate.py) and the model tests.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace vp::llm::prompts {

constexpr int kPromptVersion = 1;

// System line of both chat templates.
constexpr const char* kSystem = "You are a careful translator's assistant.";

// Template 1, closed choice. The user turn is the question followed by the options; each option is then scored
// as the complete assistant answer (option text + end-of-turn token).
inline std::string chooseUser(std::string_view question, const std::vector<std::string>& options) {
  std::string out(question);
  out += "\nOptions: ";
  for (size_t i = 0; i < options.size(); ++i) {
    if (i) out += "; ";
    out += options[i];
  }
  out += ".\nAnswer with exactly one of the options and nothing else.";
  return out;
}

// Template 2, simplification. The instruction is given in the input's language; the answer is always English.
constexpr const char* kSimplifyEn =
    "Rewrite the sentence in simple, literal English, keeping its meaning; one sentence; no commentary.";
constexpr const char* kSimplifyEs =
    "Reescribe la oración en inglés sencillo y literal, conservando su significado; una sola oración; "
    "sin comentarios. Responde en inglés.";

inline std::string simplifyUser(std::string_view sentence, bool spanish) {
  std::string out(spanish ? kSimplifyEs : kSimplifyEn);
  out += "\n\n";
  out += sentence;
  return out;
}

// GBNF for simplify: printable ASCII letters, digits, space and basic punctuation, starting with a letter or a
// digit; no newline can be produced, so "stop at newline" is enforced by the grammar itself.
constexpr const char* kSimplifyGrammar = "root ::= [A-Za-z0-9] [A-Za-z0-9 ,.;:!?'\"()-]*\n";

// Prefix of the minimal-pair gate (raw text, no chat template); the continuation is " <Sentence>.".
constexpr const char* kGatePrefix = "Latin sentence:";

// Result of the Latin minimal-pair gate (PREPLAN 2.2) measured with this prompt version on the shipped model file;
// numbers in tools/eval/llm_gate_report.json and docs/llm_notes.md. Latin-side reranking ships only when true.
constexpr bool kLatinRerankGatePassed = false;

}  // namespace vp::llm::prompts
