#include <opus/opus.h>

#include "macros/assert.hpp"
#include "opus.hpp"

namespace opus {
auto Encoder::init(const uint32_t freq) -> bool {
    auto error = 0;
    state      = AutoOpusEncoder(opus_encoder_create(freq, 1, OPUS_APPLICATION_VOIP, &error));
    ensure(state && error == OPUS_OK);
    return true;
}

auto Encoder::set_bitrate(const uint32_t bitrate) -> void {
    opus_encoder_ctl(state.get(), OPUS_SET_BITRATE(bitrate));
}

auto Encoder::encode(const std::span<const float> pcm, const size_t preserve) -> std::optional<std::vector<std::byte>> {
    auto out = std::vector<std::byte>(preserve + pcm.size() * sizeof(float));
    auto ret = opus_encode_float(state.get(), pcm.data(), pcm.size(), (unsigned char*)out.data() + preserve, out.size() - preserve);
    ensure(ret > 0, "ret={}", ret);
    out.resize(preserve + ret);
    return out;
}

auto Decoder::init(const uint32_t freq) -> bool {
    auto error = 0;
    state      = AutoOpusDecoder(opus_decoder_create(freq, 1, &error));
    ensure(state && error == OPUS_OK);
    return true;
}

auto Decoder::decode(const std::span<const std::byte> packet) -> std::optional<std::vector<float>> {
    auto out = std::vector<float>(5760);
    auto ret = opus_decode_float(state.get(), (unsigned char*)packet.data(), packet.size(), out.data(), out.size(), 0);
    ensure(ret > 0, "ret={}", ret);
    out.resize(ret);
    return out;
}

auto Decoder::packet_loss(const size_t samples) -> std::optional<std::vector<float>> {
    auto out = std::vector<float>(samples);
    auto ret = opus_decode_float(state.get(), NULL, 0, out.data(), out.size(), 0);
    ensure(ret > 0, "ret={}", ret);
    return out;
}
} // namespace opus
