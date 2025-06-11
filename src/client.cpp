#include <coop/thread.hpp>

#include "config.hpp"
#include "jitter-buffer.hpp"
#include "macros/logger.hpp"
#include "net.hpp"
#include "net/packet-parser.hpp"
#include "net/tcp/client.hpp"
#include "opus.hpp"
#include "protocol.hpp"
#include "sound.hpp"
#include "util/argument-parser.hpp"
#include "util/critical.hpp"

#define CUTIL_MACROS_PRINT_FUNC(...) LOG_ERROR(logger, __VA_ARGS__)
#include "macros/coop-unwrap.hpp"

namespace {
auto logger = Logger("CHEAPTALK");

template <class T>
auto ceil(T a, T b) -> T {
    return (a + b - 1) / b;
}

template <class T>
auto append(std::vector<T>& vec, std::span<const T> span) -> void {
    const auto prev = vec.size();
    vec.resize(prev + span.size());
    std::memcpy(vec.data() + prev, span.data(), span.size() * sizeof(T));
}

struct ReadSerial {
    static auto is_valid(const net::BytesArray& item) -> bool {
        return !item.empty();
    }

    static auto read_serial(const net::BytesArray& item) -> size_t {
        return std::bit_cast<proto::ComposedSamplesHeader*>(item.data())->serial;
    }
};

struct Context {
    using JitterBuffer = JitterBuffer<net::BytesArray, ReadSerial, config::jitter_buffer_slots>;

    net::PacketParser          parser;
    net::tcp::TCPClientBackend control_sock;
    FileDescriptor             data_sock;
    sockaddr_in                upload_addr;
    std::vector<float>         upload_buffer;
    uint64_t                   upload_serial : 56 = 0;
    uint64_t                   peer_id : 8;
    Critical<JitterBuffer>     critical_download_buffer;
    std::thread                download_thread;
    std::vector<float>         playback_reminder;
    opus::Encoder              encoder;
    opus::Decoder              decoder;

    auto download_main() -> void;

    auto init() -> coop::Async<bool>;
    auto upload_samples() -> bool;
};

auto Context::download_main() -> void {
loop:
#define error_act goto loop
    auto packet = net::BytesArray(config::max_mtu);
    auto addr   = sockaddr_storage();
    auto len    = socklen_t(sizeof(addr));

    const auto read = recvfrom(data_sock.as_handle(), packet.data(), packet.size(), 0, (sockaddr*)&addr, &len);
    // TODO: validate sender
    // ensure_a(addr.ss_family == AF_INET && ((sockaddr_in*)&addr)->sin_addr.s_addr == htonl(net::sock::build_ipv4_addr(127, 0, 0, 1)));
    ensure_a(read > ssize_t(sizeof(proto::ComposedSamplesHeader)));
    packet.resize(read);
    LOG_DEBUG(logger, "downloaded {} bytes", read);

    auto [lock, download_buffer] = critical_download_buffer.access();
    download_buffer.push(std::move(packet));
#undef error_act
    goto loop;
}

auto server_addr         = "127.0.0.1";
auto server_control_port = uint16_t(config::default_server_control_port);
auto server_data_port    = uint16_t(config::default_server_data_port);
auto client_data_port    = uint16_t(config::default_client_data_port);
auto peer_name           = "peer";

auto Context::init() -> coop::Async<bool> {
    parser.send_data = [this](const net::BytesRef payload) -> coop::Async<bool> {
        constexpr auto error_value = false;
        co_ensure_v(co_await control_sock.send(payload));
        co_return true;
    };
    control_sock.on_received = [this](net::BytesRef data) -> coop::Async<void> {
        if(const auto p = parser.parse_received(data)) {
            const auto [header, payload] = *p;
            coop_ensure(co_await parser.callbacks.invoke(header, payload));
        }
        co_return;
    };
    control_sock.on_closed = [] {
        LOG_WARN(logger, "disconnected");
        std::quick_exit(1);
    };
    coop_ensure(co_await control_sock.connect(server_addr, server_control_port));
    coop_unwrap(server_addr_v4, get_socket_addr(control_sock.sock.fd));
    coop_unwrap(response, co_await parser.receive_response<proto::PeerID>(proto::Join{peer_name, client_data_port}));
    peer_id = response.id;
    LOG_INFO(logger, "obtained peer id {}", response.id);
    coop_unwrap_mut(sock, create_udp_socket(0, client_data_port));
    data_sock   = std::move(sock);
    upload_addr = create_sock_addr(server_addr_v4, server_data_port);

    coop_ensure(encoder.init(config::encode_rate));
    coop_ensure(decoder.init(config::encode_rate));

    download_thread = std::thread(&Context::download_main, this);

    co_return true;
}

auto Context::upload_samples() -> bool {
    while(upload_buffer.size() >= config::samples_per_packet) {
        const auto to_encode = std::span{upload_buffer.data(), config::samples_per_packet};
        unwrap(packet, encoder.encode(to_encode, sizeof(proto::SamplesHeader)));
        upload_buffer.erase(upload_buffer.begin(), upload_buffer.begin() + config::samples_per_packet);
        {
            const auto bytes_before = to_encode.size() * sizeof(float);
            const auto bytes_after  = packet.size() - sizeof(proto::SamplesHeader);
            LOG_DEBUG(logger, "encoded {} bytes to {} bytes({}%)", bytes_before, bytes_after, 100.0 * bytes_after / bytes_before);
        }

        auto& header   = *std::bit_cast<proto::SamplesHeader*>(packet.data());
        header.serial  = (upload_serial += 1);
        header.peer_id = peer_id;
        const auto ret = sendto(data_sock.as_handle(), packet.data(), packet.size(), 0, (sockaddr*)&upload_addr, sizeof(upload_addr));
        if(ret != ssize_t(packet.size())) {
            LOG_ERROR(logger, "failed to upload packet ret={} errno={}({})", ret, errno, strerror(errno));
        }
    }
    return true;
}

auto context = Context();
} // namespace

namespace sound {
auto on_capture(const float* const buffer, const size_t num_samples, const size_t num_channels) -> void {
    static_assert(config::capture_rate >= config::encode_rate);
    static_assert(config::capture_rate % config::encode_rate == 0);
    constexpr auto rate = config::capture_rate / config::encode_rate;

    auto&      buf         = context.upload_buffer;
    const auto prev_size   = buf.size();
    const auto new_samples = ceil(num_samples, rate * num_channels);
    buf.resize(prev_size + new_samples);
    for(auto i = 0uz; i < new_samples; i += 1) {
        const auto src     = i * rate /*downsample*/ * num_channels /*only use first channel*/;
        buf[prev_size + i] = buffer[src];
    }
    context.upload_samples();
}

auto on_playback(float* const buffer, const size_t num_samples) -> size_t {
    constexpr auto error_value = 0;

    auto& rem = context.playback_reminder;
    LOG_DEBUG(logger, "playback request={} rem={}", num_samples, rem.size());
    if(rem.size() >= num_samples) {
        // no need to touch jitter buffer
        goto copy;
    }
    {
        auto [lock, download_buffer] = context.critical_download_buffer.access();
        auto& stage                  = download_buffer.data[0];
        if(stage.empty()) {
            unwrap_v(samples, context.decoder.packet_loss(config::samples_per_packet));
            append<float>(rem, samples);
            LOG_WARN(logger, "buffer underrun");
        } else {
            const auto to_decode = std::span{stage}.subspan(sizeof(proto::ComposedSamplesHeader));
            unwrap_v(samples, context.decoder.decode(to_decode));
            append<float>(rem, samples);
            LOG_DEBUG(logger, "shifted rem={}", rem.size());
            download_buffer.shift(1);
        }
    }
copy:
    const auto to_copy = std::min(num_samples, rem.size());
    std::memcpy(buffer, rem.data(), to_copy * sizeof(float));
    rem.erase(rem.begin(), rem.begin() + to_copy);
    return to_copy;
}
} // namespace sound

namespace {
auto async_main() -> coop::Async<bool> {
    coop_ensure(co_await context.init());
    coop_unwrap_mut(sound_context, sound::init(config::capture_rate, config::encode_rate));
    co_await coop::run_blocking([&] { sound::run(&sound_context); });
    sound::finish(&sound_context);
    co_return true;
}
} // namespace

auto main(const int argc, const char* const* argv) -> int {
    {
        auto parser = args::Parser<uint16_t>();
        auto help   = false;
        parser.kwarg(&server_addr, {"-s", "--server"}, "ADDRESS", "address of server", {.state = args::State::DefaultValue});
        parser.kwarg(&server_control_port, {"-sc", "--server-control-port"}, "PORT", "control port of server", {.state = args::State::DefaultValue});
        parser.kwarg(&server_data_port, {"-sd", "--server-data-port"}, "PORT", "data port of server", {.state = args::State::DefaultValue});
        parser.kwarg(&client_data_port, {"-cd", "--client-data-port"}, "PORT", "data port to use", {.state = args::State::DefaultValue});
        parser.kwarg(&peer_name, {"-n", "--name"}, "NAME", "peer name", {.state = args::State::DefaultValue});
        parser.kwflag(&help, {"-h", "--help"}, "print this help message", {.no_error_check = true});
        if(!parser.parse(argc, argv) || help) {
            std::println("usage: cheaptalk {}", parser.get_help());
            return 0;
        }
    }
    auto runner = coop::Runner();
    runner.push_task(async_main());
    runner.run();
    return 0;
}
