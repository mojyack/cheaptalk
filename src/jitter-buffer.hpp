#pragma once
#include <array>

#include "macros/logger.hpp"

template <class T, class S>
concept item_adapter = requires(const T& item) {
    { S::is_valid(item) } -> std::same_as<bool>;
    { S::read_serial(item) } -> std::same_as<size_t>;
};

template <class T, class S, size_t N>
    requires(item_adapter<T, S>)
struct JitterBuffer {
    static inline auto logger = Logger("CHEAPTALK_JB");

    size_t           head_serial = 0;
    std::array<T, N> data;

    auto shift(const size_t count) -> void {
        auto iter = std::shift_left(data.begin(), data.end(), count);
        for(; iter < data.end(); iter += 1) {
            *iter = {};
        }
        head_serial += count;
    }

    auto push(T item) -> void {
        const auto serial = S::read_serial(item);
        if(serial < head_serial) {
            LOG_WARN(logger, "too late, discarding serial={} head={}", serial, head_serial);
        } else if(serial >= head_serial + N) {
            LOG_WARN(logger, "too early serial={} head={}", serial, head_serial);
            // shift whole buffer to contain this packet
            shift(serial - (head_serial + N) + 1);
            data[serial - head_serial] = std::move(item);
        } else {
            auto& slot = data[serial - head_serial];
            if(S::is_valid(slot)) {
                LOG_WARN(logger, "duplicated, discarding serial={} head={}", serial, head_serial);
            } else {
                slot = std::move(item);
            }
        }
    }
};

