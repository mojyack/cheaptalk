#pragma once
#include <cstddef>

namespace sound {
struct Context;

auto init(size_t capture_rate, size_t playback_rate) -> Context*;
auto run(Context* context) -> void;
auto finish(Context* context) -> void;

// callbacks
auto on_capture(const float* buffer, size_t num_samples, size_t num_channels) -> void;
auto on_playback(float* buffer, size_t num_samples) -> size_t; // returns copied samples
} // namespace sound

