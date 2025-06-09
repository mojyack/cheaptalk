#pragma once
#include <array>

#include "config.hpp"
#include "net/serde/serde.hpp"

namespace proto {
struct PacketType {
    enum : uint8_t {
        Success = 0,
        Error,
        Join, // => PeerID
        PeerID,
    };
};

struct Success {
    constexpr static auto pt = PacketType::Success;
};

struct Error {
    constexpr static auto pt = PacketType::Error;
};

struct Join {
    constexpr static auto pt = PacketType::Join;

    SerdeFieldsBegin;
    std::string SerdeField(name);
    uint16_t    SerdeField(port);
    SerdeFieldsEnd;
};

struct PeerID {
    constexpr static auto pt = PacketType::PeerID;

    SerdeFieldsBegin;
    uint8_t SerdeField(id);
    SerdeFieldsEnd;
};

// UDP
struct SamplesHeader {
    uint64_t serial : 56;
    uint64_t peer_id : 8;
};

struct ComposedSamplesHeader {
    uint64_t serial;
};
} // namespace proto
