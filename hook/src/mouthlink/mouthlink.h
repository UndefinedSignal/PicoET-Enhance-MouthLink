#pragma once

#include <cstdint>

#include "expression/high_rate.h"

// Optional MouthLink source. Capture is fed by the existing FaceTrackor hook;
// no second camera client, injector or stock-engine call is involved.
namespace mouthlink {
using LogFn = void (*)(const char* message);

// Startup-only. Disabled by default; control_port=0 selects UDP 9040.
void configure(bool enabled, uint16_t control_port, LogFn log) noexcept;
// Starts the control/sender thread. Failures disable only MouthLink, never PicoET.
void start() noexcept;
// Called from FaceTrackor dispatch. Noexcept and non-blocking towards the network.
void offer(const expression_high_rate::Mat* mouth, uint64_t source_ns) noexcept;
}
