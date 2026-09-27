#pragma once
#include "client.hpp"
#include "model.hpp"

namespace lemon {
int run_tui(AppOptions options, bool launch_form);
// Plain deterministic rendering for tests and documentation; uses the real TUI model.
std::string render_demo(int width, int height);
} // namespace lemon
