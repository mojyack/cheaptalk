#include <vector>

#include "macros/assert.hpp"
#include "macros/autoptr.hpp"
#include "opus/opus.h"

namespace {
declare_autoptr(OpusEncoder, OpusEncoder, opus_encoder_destroy);
declare_autoptr(OpusDecoder, OpusDecoder, opus_decoder_destroy);
} // namespace

auto main() -> int {
    auto error   = 0;
    auto encoder = AutoOpusEncoder(opus_encoder_create(8000, 1, OPUS_APPLICATION_VOIP, &error));
    ensure(encoder && error == OPUS_OK);
    opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(1000)); // 500 ~ 512000 bps

    auto buf = std::vector<float>(320);
    auto out = std::vector<std::byte>(buf.size() * sizeof(float));

    auto ret = opus_encode_float(encoder.get(), buf.data(), buf.size(), (unsigned char*)out.data(), out.size());
    PRINT("ret={} {}%", ret, 100.0 * ret / buf.size());
    out.resize(ret);

    auto decoder = AutoOpusDecoder(opus_decoder_create(8000, 1, &error));
    ensure(decoder && error == OPUS_OK);
    ret = opus_decode_float(decoder.get(), (unsigned char*)out.data(), out.size(), buf.data(), buf.size(), 0);
    PRINT("ret={}", ret);
}
