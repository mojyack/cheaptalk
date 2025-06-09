#pragma once

// opus frame(ms): 2.5, 5, 10, 20, 40, 60

namespace config {
// audio processing
constexpr auto samples_per_packet = 320;   // encode_rate * 40ms / 1s; max: mtu / sizeof(float)
constexpr auto capture_rate       = 48000; // Hz
constexpr auto encode_rate        = 8000;  // Hz
constexpr auto compose_delay_ms   = 200;

constexpr auto packets_per_second  = encode_rate / samples_per_packet;
constexpr auto jitter_buffer_slots = packets_per_second * compose_delay_ms / 1000 + 1 /*1 for staging slot*/;

// networking
constexpr auto max_mtu                     = 1500;
constexpr auto default_server_control_port = 8000;
constexpr auto default_server_data_port    = 8001;
constexpr auto default_client_data_port    = 8010; // client side
} // namespace config
