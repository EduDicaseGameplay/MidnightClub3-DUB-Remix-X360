// MC3 (2026-11-11): a title fault on the console restarts it with nothing in the log (first console run: the log
// stopped at the logo, frame 60). Every host thread runs under Guard, and the rest goes through the unhandled
// exception filter, abort, terminate, pure calls and failed allocations: each writes what it knows to the log at once.
#pragma once
#include <functional>

namespace crash {

void Install();                         // once, first thing in main
void Guard(std::function<void()>& fn);  // runs fn; a hardware exception in it is logged
void Pulse();                           // called often from the main loop: one line a second for 60 s, then every 10 s

}  // namespace crash
