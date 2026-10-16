// Writing: copy every recorded byte back; only cues whose text changed are re-broken; then re-encode.
#include <exception>
#include <utility>

#include "internal.h"

namespace vp::subs {
namespace {

std::string flatten(const std::vector<Span>& spans) {
  std::string flat;
  for (const Span& s : spans) {
    if (s.kind == Span::Newline) flat.push_back(' ');
    else flat += s.raw;
  }
  return flat;
}

}  // namespace

Result<std::vector<uint8_t>> write(const Document& d, const WriteOptions& o, std::vector<Warning>* warnings) {
  try {
    const std::string enc = detail::canonicalEncoding(o.encoding.empty() ? d.encoding : o.encoding);
    if (enc.empty())
      return Result<std::vector<uint8_t>>(ErrorCode::BadParams, "unknown encoding: " + o.encoding,
                                          "Choose UTF-8, UTF-16 or Windows-1252.");
    const bool bom = o.bom ? *o.bom : d.bom;
    const std::string nl = d.newline.empty() ? std::string("\n") : d.newline;
    const std::string br = d.format == Format::Ass ? std::string("\\N") : nl;
    auto warn = [&](uint32_t index, const char* kind, std::string detail) {
      if (warnings) warnings->push_back(Warning{index, kind, std::move(detail)});
    };

    std::size_t estimate = d.headerRaw.size() + d.trailerRaw.size();
    for (const Cue& c : d.cues)
      estimate += c.idRaw.size() + c.timingRaw.size() + c.styleRaw.size() + c.parsedText.size() + c.tailRaw.size() + 8;
    std::string out;
    out.reserve(estimate);
    out += d.headerRaw;
    for (std::size_t k = 0; k < d.cues.size(); ++k) {
      const Cue& c = d.cues[k];
      const bool last = k + 1 == d.cues.size();
      std::string text = detail::renderSpans(c.spans, br);
      const bool changed = !c.fromSource || text != c.parsedText;
      if (changed && o.rebreak && d.format != Format::Txt && !c.spans.empty()) {
        bool overflow = false;
        std::vector<std::string> lines = breakLines(flatten(c.spans), o.hints, o.maxLine, o.maxLines, &overflow);
        if (overflow) warn(c.index, "line_overflow", "text does not fit in the allowed lines");
        text.clear();
        for (std::size_t i = 0; i < lines.size(); ++i) {
          if (i) text += br;
          text += lines[i];
        }
      }
      if (d.format == Format::Ass) {
        if (c.styleRaw.empty()) {
          out += "Dialogue: 0,";
          out += c.timingRaw;
          out += ",Default,,0,0,0,,";
        } else {
          out += c.styleRaw;
        }
        out += text;
        out += c.fromSource ? c.textEol : nl;
        out += c.tailRaw;
        continue;
      }
      if (d.format != Format::Txt) {
        if (!c.idRaw.empty()) {
          out += c.idRaw;
          out += c.fromSource ? c.idEol : nl;
        }
        out += c.timingRaw;
        out += (c.fromSource && !(c.timingEol.empty() && !text.empty())) ? c.timingEol : nl;
      }
      if (c.fromSource) {
        if (!text.empty()) {
          out += text;
          out += c.parsedText.empty() ? nl : c.textEol;
        }
        out += c.tailRaw;
      } else {
        out += text;
        if (!text.empty()) out += nl;
        if (!last) out += nl;
      }
    }
    out += d.trailerRaw;

    std::vector<uint8_t> bytes;
    std::size_t replaced = detail::encode(out, enc, bom, bytes);
    if (replaced) warn(0, "unmappable_char", std::to_string(replaced) + " character(s) written as '?' in " + enc);
    return Result<std::vector<uint8_t>>(std::move(bytes));
  } catch (const std::exception& e) {
    return Result<std::vector<uint8_t>>(ErrorCode::Internal, std::string("subtitle write failed: ") + e.what(),
                                        "The subtitle file could not be written.");
  } catch (...) {
    return Result<std::vector<uint8_t>>(ErrorCode::Internal, "subtitle write failed",
                                        "The subtitle file could not be written.");
  }
}

}  // namespace vp::subs
