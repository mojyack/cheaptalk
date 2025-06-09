#pragma once
#include <optional>
#include <span>
#include <vector>

#include "macros/autoptr.hpp"

extern "C" {
struct OpusEncoder;
struct OpusDecoder;

auto opus_encoder_destroy(OpusEncoder* st) -> void;
auto opus_decoder_destroy(OpusDecoder* st) -> void;
}

namespace opus {
declare_autoptr(OpusEncoder, OpusEncoder, opus_encoder_destroy);
declare_autoptr(OpusDecoder, OpusDecoder, opus_decoder_destroy);

struct Encoder {
    AutoOpusEncoder state;

    auto init(uint32_t freq) -> bool;
    auto set_bitrate(uint32_t bitrate) -> void; // 500 ~ 512000 bps
    auto encode(std::span<const float> pcm, size_t preserve = 0) -> std::optional<std::vector<std::byte>>;
};

struct Decoder {
    AutoOpusDecoder state;

    auto init(uint32_t freq) -> bool;
    auto decode(std::span<const std::byte> packet) -> std::optional<std::vector<float>>;
    auto packet_loss(size_t samples) -> std::optional<std::vector<float>>;
};
} // namespace opus
