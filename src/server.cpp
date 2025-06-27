#include <ranges>

#include <coop/io.hpp>
#include <coop/task-handle.hpp>
#include <coop/timer.hpp>
#include <netinet/in.h>
#include <sys/socket.h>

#include "config.hpp"
#include "jitter-buffer.hpp"
#include "macros/logger.hpp"
#include "net.hpp"
#include "net/packet-parser.hpp"
#include "net/tcp/server.hpp"
#include "opus.hpp"
#include "protocol.hpp"
#include "util/argument-parser.hpp"
#include "util/fd.hpp"

#define CUTIL_MACROS_PRINT_FUNC(...) LOG_ERROR(logger, __VA_ARGS__)
#include "macros/coop-unwrap.hpp"

namespace {
auto logger = Logger("CHEAPTALK_SERVER");

struct ReadSerial {
    static auto is_valid(const net::BytesArray& item) -> bool {
        return !item.empty();
    }

    static auto read_serial(const net::BytesArray& item) -> size_t {
        return std::bit_cast<proto::SamplesHeader*>(item.data())->serial;
    }
};

struct Peer {
    using JitterBuffer = JitterBuffer<net::BytesArray, ReadSerial, config::jitter_buffer_slots>;

    std::string   name;
    sockaddr_in   addr;
    JitterBuffer  jitter_buffer;
    opus::Encoder encoder;
    opus::Decoder decoder;
};

struct ClientData {
    net::PacketParser parser;
    int               peer_id = -1;

    ClientData(net::ServerBackend& backend, net::ClientData& client);
};

ClientData::ClientData(net::ServerBackend& backend, net::ClientData& client) {
    parser.send_data = [&backend, &client](const net::BytesRef payload) -> coop::Async<bool> {
        constexpr auto error_value = false;
        co_ensure_v(co_await backend.send(client, payload));
        co_return true;
    };
}

auto control_port = uint16_t(config::default_server_control_port);
auto data_port    = uint16_t(config::default_server_data_port);

struct Compositor {
    FileDescriptor                     data_sock;
    coop::TaskHandle                   data_reader_task;
    coop::TaskHandle                   compose_task;
    std::vector<std::unique_ptr<Peer>> peers;
    size_t                             distribute_serial = 0uz;

    auto data_reader_main() -> coop::Async<void>;
    auto compose_main() -> coop::Async<void>;
    auto allocate_peer() -> std::optional<uint8_t>;
    auto count_active_peers() const -> size_t;

    auto init() -> coop::Async<bool>;
    auto handle_payload(const net::ClientData& client_data, net::Header header, net::BytesRef payload) -> coop::Async<bool>;
    auto remove_client(ClientData& client) -> void;
};

auto Compositor::init() -> coop::Async<bool> {
    coop_unwrap_mut(sock, create_udp_socket(0, data_port));
    data_sock = std::move(sock);
    co_return true;
}

auto Compositor::data_reader_main() -> coop::Async<void> {
loop:
#define error_act goto loop
    ASSERT(!(co_await coop::wait_for_file(data_sock.as_handle(), true, false)).error, "data socket aborted");

    auto packet = net::BytesArray(config::max_mtu);
    auto addr   = sockaddr_storage();
    auto len    = socklen_t(sizeof(addr));

    const auto read = recvfrom(data_sock.as_handle(), packet.data(), packet.size(), 0, (sockaddr*)&addr, &len);
    ensure_a(read > ssize_t(sizeof(proto::SamplesHeader)));
    packet.resize(read);

    const auto& header = *std::bit_cast<proto::SamplesHeader*>(packet.data());
    ensure_a(peers.size() > header.peer_id && peers[header.peer_id]);

    auto& peer = *peers[header.peer_id];
    LOG_DEBUG(logger, "push buffer peer={}", int(header.peer_id));
    peer.jitter_buffer.push(std::move(packet));
#undef error_act
    goto loop;
}

auto Compositor::compose_main() -> coop::Async<void> {
    constexpr auto us_per_packet = 1'000'000 * config::samples_per_packet / config::encode_rate;
    constexpr auto interval      = std::chrono::microseconds(us_per_packet);

    struct DecodeCache {
        Peer*              peer;
        std::vector<float> samples;
    };

    auto next_wakeup = std::chrono::system_clock::now();
loop:
    // decode uploaded packets
    auto decoded = std::vector<DecodeCache>();
    decoded.reserve(peers.size());
    for(auto& ptr : peers) {
#define error_act continue
        if(!ptr) {
            continue;
        }
        auto& peer  = *ptr;
        auto& stage = peer.jitter_buffer.data[0];
        auto& cache = decoded.emplace_back();
        cache.peer  = &peer;
        if(stage.empty()) {
            LOG_WARN(logger, "peer {} has no packet to send", peer.name);
            unwrap_a_mut(samples, peer.decoder.packet_loss(config::samples_per_packet));
            cache.samples = std::move(samples);
        } else {
            const auto to_decode = std::span{stage}.subspan(sizeof(proto::SamplesHeader));
            unwrap_a_mut(samples, peer.decoder.decode(to_decode));
            ensure_a(samples.size() == config::samples_per_packet);
            cache.samples = std::move(samples);
            peer.jitter_buffer.shift(1);
        }
#undef error_act
    }

    // compose once
    auto composed = std::vector<float>(config::samples_per_packet);
    for(auto& cache : decoded) {
        for(auto&& [sample, composed] : std::views::zip(cache.samples, composed)) {
            composed += sample;
        }
    }

    // distribute samples
    for(auto& cache : decoded) {
#define error_act continue
        auto& peer      = *cache.peer;
        auto  to_encode = std::vector<float>(config::samples_per_packet);
        // exclude itself
        for(auto&& [sample, composed, encode] : std::views::zip(cache.samples, composed, to_encode)) {
            encode = composed - sample;
        }
        unwrap_a(packet, peer.encoder.encode(to_encode, sizeof(proto::ComposedSamplesHeader)));

        // send
        auto& header   = *std::bit_cast<proto::ComposedSamplesHeader*>(packet.data());
        header.serial  = distribute_serial;
        const auto ret = sendto(data_sock.as_handle(), packet.data(), packet.size(), 0, (sockaddr*)&peer.addr, sizeof(peer.addr));
        ensure_a(ret == ssize_t(packet.size()), "failed to send packet to peer {} ret={} errno={}({})", peer.name, ret, errno, strerror(errno));
#undef error_act
    }
    distribute_serial += 1;

    // sync to clock
    next_wakeup += interval;
    const auto loop_end = std::chrono::system_clock::now();
    if(next_wakeup > loop_end) {
        const auto free = std::chrono::duration_cast<std::chrono::microseconds>(next_wakeup - loop_end);
        LOG_DEBUG(logger, "compose done, free={:.2}%", 100.0 * free.count() / interval.count());
        co_await coop::sleep(next_wakeup - loop_end);
    } else {
        const auto over = std::chrono::duration_cast<std::chrono::microseconds>(loop_end - next_wakeup);
        LOG_ERROR(logger, "composition not completed in time, over={:.2}%", 100.0 * over.count() / interval.count());
        next_wakeup = loop_end;
    }
    goto loop;
}

auto Compositor::allocate_peer() -> std::optional<uint8_t> {
    auto id = -1;
    for(auto i = 0uz; i < peers.size(); i += 1) {
        if(!peers[i]) {
            id = i;
            break;
        }
    }
    if(id == -1) {
        ensure(peers.size() <= 0xff, "too many peers");
        peers.emplace_back();
        id = peers.size() - 1;
    }
    return id;
}

auto Compositor::count_active_peers() const -> size_t {
    auto count = 0uz;
    for(auto& peer : peers) {
        if(peer) {
            count += 1;
        }
    }
    return count;
}

auto Compositor::handle_payload(const net::ClientData& client_data, const net::Header header, const net::BytesRef payload) -> coop::Async<bool> {
    auto& client = *(ClientData*)client_data.data;
    switch(header.type) {
    case proto::Join::pt: {
        coop_unwrap_mut(request, (serde::load<net::BinaryFormat, proto::Join>(payload)));
        coop_ensure(client.peer_id == -1);

        const auto sock_fd = std::bit_cast<net::sock::SocketClientData*>(&client_data)->sock.fd;
        coop_unwrap(addr, get_peer_addr(sock_fd));
        coop_unwrap(id, allocate_peer());
        LOG_INFO(logger, "new peer name={} id={} addr={:X}:{}", request.name, id, addr, request.port);

        auto peer  = std::unique_ptr<Peer>(new Peer());
        peer->name = std::move(request.name);
        peer->addr = create_sock_addr(addr, request.port);
        coop_ensure(peer->encoder.init(config::encode_rate));
        coop_ensure(peer->decoder.init(config::encode_rate));
        peers[id]      = std::move(peer);
        client.peer_id = id;

        coop_ensure(co_await client.parser.send_packet(proto::PeerID{id}, header.id));
        if(count_active_peers() == 2) {
            LOG_INFO(logger, "starting compose task");
            auto& runner = *co_await coop::reveal_runner();
            runner.push_task(data_reader_main(), &data_reader_task);
            runner.push_task(compose_main(), &compose_task);
        }
        co_return true;
    }
    default:
        coop_bail("unhandled packet type {}", header.type);
    }
}

auto Compositor::remove_client(ClientData& client) -> void {
    if(client.peer_id == -1) {
        return;
    }
    ensure(peers[client.peer_id]);
    auto& peer = *peers[client.peer_id];
    LOG_INFO(logger, "remove peer name={} id={}", peer.name, client.peer_id);
    peers[client.peer_id].reset();
    if(count_active_peers() == 1) {
        LOG_INFO(logger, "stopping compose task");
        compose_task.cancel();
        data_reader_task.cancel();
    }
}

auto async_main() -> coop::Async<bool> {
    // setup compositor
    auto compositor = Compositor();
    coop_ensure(co_await compositor.init());

    // setup control socket
    auto server         = net::tcp::TCPServerBackend();
    server.alloc_client = [&server](net::ClientData& client) -> coop::Async<void> {
        client.data = new ClientData(server, client);
        co_return;
    };
    server.free_client = [&compositor](void* ptr) -> coop::Async<void> {
        auto& client = *(ClientData*)ptr;
        compositor.remove_client(client);
        delete &client;
        co_return;
    };
    server.on_received = [&compositor](const net::ClientData& client, net::BytesRef data) -> coop::Async<void> {
        auto& c = *(ClientData*)client.data;
        if(const auto p = c.parser.parse_received(data)) {
            if(!co_await compositor.handle_payload(client, p->header, p->payload)) {
                co_await c.parser.send_packet(proto::Error(), p->header.id);
            }
        }
    };
    coop_ensure(co_await server.start(control_port));

    // wait until finished
    co_await server.task.join();
    co_return true;
}
} // namespace

auto main(const int argc, const char* const* argv) -> int {
    {
        auto parser = args::Parser<uint16_t>();
        auto help   = false;
        parser.kwarg(&control_port, {"-sc", "--control-port"}, "PORT", "control port to use", {.state = args::State::DefaultValue});
        parser.kwarg(&data_port, {"-sd", "--data-port"}, "PORT", "data port to use", {.state = args::State::DefaultValue});
        parser.kwflag(&help, {"-h", "--help"}, "print this help message", {.no_error_check = true});
        if(!parser.parse(argc, argv) || help) {
            std::println("usage: cheaptalk-server {}", parser.get_help());
            return 0;
        }
    }

    auto runner = coop::Runner();
    runner.push_task(async_main());
    runner.run();
    return 0;
}
