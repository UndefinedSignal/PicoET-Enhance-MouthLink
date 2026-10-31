#pragma once

#include <cstdint>

#include "expression/high_rate.h"

// Optional MouthLink source. Capture is fed by the existing FaceTrackor hook;
// no second camera client, injector or stock-engine call is involved.
namespace mouthlink {
using LogFn = void (*)(const char* message);

// Startup-only. Disabled by default; control_port=0 selects UDP 9040.
// allow: control-packet sources, "private" (default when empty/null), "any", or IPv4
// addresses / CIDR ranges separated by spaces or commas; a malformed list allows nothing.
void configure(bool enabled, uint16_t control_port, const char* allow, LogFn log) noexcept;
// Starts the control/sender thread. Failures disable only MouthLink, never PicoET.
void start() noexcept;
// Called from FaceTrackor dispatch. Noexcept and non-blocking towards the network.
void offer(const expression_high_rate::Mat* mouth, uint64_t source_ns) noexcept;
// Called from the expression thread after the stock mouth filter accepted a new
// source. raw35 = stock network output, instant33 = post-processed but unsmoothed,
// final33 = smoothed output exported to the bridge; both 33-vectors in mouth33 order.
// Non-blocking; queued only while a subscriber asked for STOCK.
void offer_stock(uint64_t source_ns, const float raw35[35], const float instant33[33],
                 const float final33[33], bool valid, bool warmup, int8_t classification) noexcept;
}
