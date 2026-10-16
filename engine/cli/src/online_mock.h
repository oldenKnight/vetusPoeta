// TESTS ONLY: a scripted Wiktionary transport for `vpengine serve` when VP_ONLINE_MOCK is set (engine iii without
// the network). It answers the REST definition URL of a Latin title with the lexicon's own English gloss as the
// definition (so the verdict is "agrees"); VP_ONLINE_MOCK=disagree answers every title with an unrelated definition,
// and a title the lexicon does not know gets HTTP 404. It never opens a socket or starts a process.
#pragma once
#include <atomic>
#include <memory>

#include "vp/lex.h"
#include "vp/online.h"

namespace vpcli {

std::unique_ptr<vp::online::Transport> makeMockTransport(const vp::lex::Lexicon* latin, bool disagree,
                                                         std::atomic<uint64_t>* calls);

}  // namespace vpcli
