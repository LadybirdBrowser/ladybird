/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fuzzing/AsyncSupport.h"
#include "Fuzzing/BoundedInput.h"
#include "Fuzzing/Dispatch.h"
#include <AK/Array.h>
#include <LibIPC/File.h>
#include <LibURL/Parser.h>
#include <RequestServer/CURL.h>
#include <RequestServer/ControlConnectionFromClient.h>
#include <RequestServer/ResourceSubstitutionMap.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>

namespace RequestServer {

OwnPtr<ResourceSubstitutionMap> g_resource_substitution_map;

}

namespace {

// A controlled HTTP peer, not a target. No response is released until all three
// requests arrived and the ownership operations have completed on the main loop.
class Fixture {
public:
    Fixture()
        : listener(IPC::File::adopt_fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)))
    {
        VERIFY(listener.fd() >= 0);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        VERIFY(::bind(listener.fd(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        VERIFY(::listen(listener.fd(), 4) == 0);
        socklen_t length = sizeof(address);
        VERIFY(::getsockname(listener.fd(), reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port = ntohs(address.sin_port);
    }

    struct Socket {
        IPC::File fd;
        Array<char, 4096> request {};
        size_t used { 0 };
        Optional<size_t> resource {};
        ByteString response {};
        size_t sent { 0 };
    };

    void progress()
    {
        for (;;) {
            int fd = ::accept4(listener.fd(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd < 0) {
                VERIFY(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
                break;
            }
            VERIFY(sockets.size() < 3);
            sockets.append(Socket { .fd = IPC::File::adopt_fd(fd) });
        }
        for (auto& socket : sockets) {
            if (!socket.resource.has_value()) {
                VERIFY(socket.used < socket.request.size());
                auto count = ::recv(socket.fd.fd(), socket.request.data() + socket.used, socket.request.size() - socket.used, 0);
                if (count > 0)
                    socket.used += count;
                else if (count < 0)
                    VERIFY(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
                StringView request(socket.request.data(), socket.used);
                if (request.contains("\r\n\r\n"sv)) {
                    for (size_t index = 0; index < 3; ++index) {
                        if (request.starts_with(ByteString::formatted("GET /body/{} HTTP/1.1\r\n", index))) {
                            socket.resource = index;
                            VERIFY(!arrived[index]);
                            arrived[index] = true;
                            socket.response = ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Length: {}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n{}", bodies[index].length(), bodies[index]);
                            break;
                        }
                    }
                    VERIFY(socket.resource.has_value());
                }
            }
            if (released && socket.resource.has_value() && socket.sent < socket.response.length()) {
                auto count = ::send(socket.fd.fd(), socket.response.characters() + socket.sent, min(chunk_size, socket.response.length() - socket.sent), MSG_NOSIGNAL);
                if (count > 0) {
                    socket.sent += count;
                } else if (count < 0 && (errno == EPIPE || errno == ECONNRESET)) {
                    socket.sent = socket.response.length(); // An intentionally stopped request.
                } else {
                    VERIFY(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
                }
            }
        }
    }

    IPC::File listener;
    u16 port { 0 };
    Vector<Socket> sockets;
    Array<ByteString, 3> bodies;
    Array<bool, 3> arrived {};
    bool released { false };
    size_t chunk_size { 1 };
};

struct Observation {
    Optional<size_t> resource;
    IPC::File pipe;
    ByteBuffer body;
    bool must_fail { false };
    bool transferred { false };
    bool cancelled { false };
    bool finished { false };
};

}

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    AK::set_debug_enabled(false);
    if (size > 4096)
        return 0;
    static bool initialized = [] {
        for (auto const* name : { "http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY" })
            VERIFY(::unsetenv(name) == 0);
        MUST(RequestServer::initialize_libcurl());
        return true;
    }();
    (void)initialized;
    Core::EventLoop loop;
    Fixture fixture;
    Fuzzing::BoundedInput input({ data, size });
    fixture.chunk_size = 1 + input.byte() % 64;
    for (size_t index = 0; index < 3; ++index)
        fixture.bodies[index] = ByteString::formatted("recipient-{}-value-{}", index, input.byte());

    using Client = RequestServer::ConnectionFromClient;
    using namespace Messages::RequestServer;
    Client::ConnectionMap connections;
    Client::RequestTransferLeaseMap leases;
    auto control_pair = MUST(IPC::Transport::create_paired());
    auto control_peer = MUST(control_pair.remote_handle.create_transport());
    RefPtr<RequestServer::ControlConnectionFromClient> control = RequestServer::ControlConnectionFromClient::construct(move(control_pair.local), connections, leases, Optional<HTTP::DiskCache&> {}, ByteString {});
    Array<RefPtr<Client>, 2> clients;
    Array<OwnPtr<IPC::Transport>, 2> peers;
    Array<Array<Observation, 32>, 2> observed;
    for (size_t owner = 0; owner < 2; ++owner) {
        auto privacy = owner ? RequestServer::IsPrivate::Yes : RequestServer::IsPrivate::No;
        auto response = Fuzzing::dispatch(*control, Messages::RequestServerControl::ConnectNewClient { privacy });
        peers[owner] = MUST(response.handle().create_transport());
        for (auto const& entry : connections)
            if (entry.value->is_private() == privacy)
                clients[owner] = entry.value;
        VERIFY(clients[owner]);
    }
    VERIFY(connections.size() == 2 && clients[0]->client_id() != clients[1]->client_id());
    // The control endpoint is trusted test setup. A data decoder must not admit
    // its client-minting message, even though the same process implements both.
    auto forbidden = MUST(Messages::RequestServerControl::ConnectNewClients(1, RequestServer::IsPrivate::Yes).encode());
    Queue<IPC::Attachment> attachments;
    VERIFY(RequestServerEndpoint::decode_message(forbidden.data().span(), attachments).is_error());
    VERIFY(connections.size() == 2);

    auto drain = [&] {
        fixture.progress();
        for (size_t owner = 0; owner < 2; ++owner) {
            Fuzzing::receive<RequestClientEndpoint>(*peers[owner], [&](IPC::Message& message) {
                using namespace Messages::RequestClient;
                if (message.message_id() == RequestStarted::static_message_id()) {
                    auto const& started = static_cast<RequestStarted const&>(message);
                    VERIFY(started.request_id() < 32);
                    auto& entry = observed[owner][started.request_id()];
                    VERIFY(entry.resource.has_value() && !entry.must_fail && entry.pipe.fd() < 0);
                    entry.pipe = IPC::File::adopt_fd(started.fd().take_fd());
                    int flags = ::fcntl(entry.pipe.fd(), F_GETFL);
                    VERIFY(flags >= 0 && ::fcntl(entry.pipe.fd(), F_SETFL, flags | O_NONBLOCK) == 0);
                } else if (message.message_id() == RequestTransferred::static_message_id()) {
                    auto id = static_cast<RequestTransferred const&>(message).request_id();
                    VERIFY(id < 32 && observed[owner][id].resource.has_value());
                    observed[owner][id].transferred = true;
                    // Follow the real client's transfer contract. Retained old FDs
                    // are not claimed to be revoked by this bookkeeping protocol.
                    observed[owner][id].pipe = {};
                } else if (message.message_id() == RequestFinished::static_message_id()) {
                    auto const& result = static_cast<RequestFinished const&>(message);
                    VERIFY(result.request_id() < 32);
                    auto& entry = observed[owner][result.request_id()];
                    VERIFY(entry.resource.has_value() || entry.must_fail);
                    VERIFY(!entry.finished && !entry.transferred);
                    VERIFY(result.network_error().has_value() == entry.must_fail);
                    entry.finished = true;
                    if (!entry.must_fail)
                        VERIFY(result.total_size() == fixture.bodies[*entry.resource].length());
                } else if (message.message_id() == HeadersBecameAvailable::static_message_id()) {
                    auto const& result = static_cast<HeadersBecameAvailable const&>(message);
                    VERIFY(result.request_id() < 32 && result.status_code() == 200u);
                    VERIFY(observed[owner][result.request_id()].resource.has_value());
                } else {
                    VERIFY_NOT_REACHED(); // No credentials, cache files, WebSocket or TLS in this lane.
                }
            });
            for (auto& entry : observed[owner]) {
                if (entry.pipe.fd() < 0 || entry.transferred || entry.cancelled)
                    continue;
                Array<u8, 128> bytes;
                auto count = ::read(entry.pipe.fd(), bytes.data(), bytes.size());
                if (count > 0) {
                    MUST(entry.body.try_append(bytes.data(), count));
                    VERIFY(entry.resource.has_value() && entry.body.size() <= fixture.bodies[*entry.resource].length());
                } else if (count < 0) {
                    VERIFY(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
                }
            }
        }
    };
    for (size_t resource = 0; resource < 3; ++resource) {
        size_t owner = resource == 1 ? 1 : 0;
        u64 id = resource + 1;
        observed[owner][id].resource = resource;
        auto url = URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}/body/{}", fixture.port, resource)).release_value();
        Fuzzing::dispatch(*clients[owner], StartRequest { id, "GET", move(url), {}, {}, HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::No, resource == 0, {}, false, 0, 0 });
    }
    Fuzzing::pump_until(loop, [&] { return fixture.arrived[0] && fixture.arrived[1] && fixture.arrived[2]; }, drain, "HTTP request arrival controls");

    size_t current_owner = 0;
    u64 current_id = 1;
    bool active = true;
    bool lease_live = true;
    int source = clients[0]->client_id();
    for (u64 step = 0; step < 8 && !input.remaining().is_empty(); ++step) {
        size_t actor = input.byte() % 2;
        auto operation = input.byte() % 6;
        u64 target = 10 + step;
        if (operation == 0 || operation == 2 || operation == 5) {
            bool valid = operation == 0 && active && lease_live;
            auto& observation = observed[actor][target];
            observation.must_fail = !valid;
            if (valid)
                observation.resource = 0;
            Fuzzing::dispatch(*clients[actor], AdoptRequest { source, operation == 2 ? 999u : (operation == 5 ? 3u : 1u), target, true });
            if (valid) {
                // Mark before draining, so a late duplicate old callback cannot
                // silently become part of the new owner's observation.
                observed[current_owner][current_id].transferred = true;
                observed[current_owner][current_id].pipe = {};
                current_owner = actor;
                current_id = target;
            }
        } else if (operation == 1) {
            Fuzzing::dispatch(*clients[actor], ReleaseRequestTransferLease { source, 1 });
            if (actor == current_owner)
                lease_live = false;
        } else if (operation == 3 && active) {
            auto result = Fuzzing::dispatch(*clients[actor], StopRequest { current_id });
            VERIFY(result.success() == (actor == current_owner));
            if (actor == current_owner) {
                observed[current_owner][current_id].cancelled = true;
                active = false;
                lease_live = false;
            }
        }
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        drain();
    }
    fixture.released = true;
    auto complete = [&] {
        for (size_t owner = 0; owner < 2; ++owner) {
            for (auto const& entry : observed[owner]) {
                if (entry.transferred || entry.cancelled || (!entry.resource.has_value() && !entry.must_fail))
                    continue;
                if (!entry.finished || (entry.resource.has_value() && entry.body.size() != fixture.bodies[*entry.resource].length()))
                    return false;
            }
        }
        return true;
    };
    Fuzzing::pump_until(loop, complete, drain, "HTTP delivery/recipient barrier");
    for (auto const& owner : observed)
        for (auto const& entry : owner)
            if (entry.finished && entry.resource.has_value())
                VERIFY(entry.body.bytes() == fixture.bodies[*entry.resource].bytes());
    Array<WeakPtr<Client>, 2> retired_clients { clients[0], clients[1] };
    for (auto& client : clients) {
        client->shutdown();
        client = nullptr;
    }
    VERIFY(connections.is_empty() && leases.is_empty());
    auto retired_control = control->make_weak_ptr();
    control->shutdown();
    control = nullptr;
    control_peer->close();
    for (auto& peer : peers)
        peer = nullptr;
    // Deferred IPC shutdown callbacks retain their receivers. Destroy every receiver
    // while its borrowed connection/lease maps still exist, before the next input.
    Fuzzing::pump_until(loop, [&] { return !retired_control && !retired_clients[0] && !retired_clients[1]; }, [] {}, "RequestServer receiver teardown");
    return 0;
}
