/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fuzzing/BoundedInput.h"
#include "Fuzzing/Dispatch.h"
#include <AK/Array.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/File.h>
#include <LibURL/Parser.h>
#include <RequestServer/CURL.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/ResourceSubstitutionMap.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>

// Normally provided by the executable, not the production receiver library.
namespace RequestServer {

OwnPtr<ResourceSubstitutionMap> g_resource_substitution_map;

}

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    AK::set_debug_enabled(false);
    if (size > 4096)
        return 0;
    static bool initialized = [] {
        // libcurl otherwise honors ambient proxy settings even for our fixture.
        for (auto const* name : { "http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY" })
            VERIFY(::unsetenv(name) == 0);
        MUST(RequestServer::initialize_libcurl());
        return true;
    }();
    (void)initialized;
    Core::EventLoop loop;
    using Client = RequestServer::ConnectionFromClient;
    Client::ConnectionMap connections;
    Client::RequestTransferLeaseMap leases;
    Array<RefPtr<Client>, 2> clients;
    Array<OwnPtr<IPC::Transport>, 2> peers;
    Vector<WeakPtr<Client>> generations;
    auto replace_client = [&](size_t index) {
        if (clients[index])
            clients[index]->shutdown();
        peers[index] = nullptr;
        auto pair = MUST(IPC::Transport::create_paired());
        peers[index] = MUST(pair.remote_handle.create_transport());
        clients[index] = Client::construct(move(pair.local), RequestServer::IsPrivate::Yes, connections, leases, Optional<HTTP::DiskCache&> {}, ByteString {});
        generations.append(clients[index]->make_weak_ptr<Client>());
    };
    replace_client(0);
    replace_client(1);

    // Own the only possible network destination. No response is supplied: this
    // lane targets request ownership before headers/body delivery, not HTTP parsing.
    int listener_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    VERIFY(listener_fd >= 0);
    auto listener = IPC::File::adopt_fd(listener_fd);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    VERIFY(::bind(listener.fd(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    VERIFY(::listen(listener.fd(), 64) == 0);
    socklen_t address_size = sizeof(address);
    VERIFY(::getsockname(listener.fd(), reinterpret_cast<sockaddr*>(&address), &address_size) == 0);
    auto url = URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}/lease", ntohs(address.sin_port))).release_value();

    struct Model {
        Requests::RequestTransferLeaseKey key;
        size_t owner;
        u64 request;
        bool live;
    };
    Vector<Model> model;
    Fuzzing::BoundedInput input({ data, size });
    using namespace Messages::RequestServer;
    for (u64 step = 0; step < 48 && (step < 2 || !input.remaining().is_empty()); ++step) {
        size_t actor = input.byte() % 2;
        auto operation = step < 2 ? 0 : input.byte() % 5;
        u64 fresh_request = step + 1;
        auto& client = *clients[actor];
        if (operation == 0 || model.is_empty()) {
            Fuzzing::dispatch(client, StartRequest { fresh_request, "GET", url, {}, {}, HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::No, true, {}, false, 0, 0 });
            model.append({ { client.client_id(), fresh_request }, actor, fresh_request, true });
        } else {
            auto& selected = model[input.byte() % model.size()];
            switch (operation) {
            case 1: {
                bool preserve = input.byte() & 1;
                Fuzzing::dispatch(client, AdoptRequest { selected.key.source_client_id, selected.key.source_request_id, fresh_request, preserve });
                if (selected.live) {
                    selected.owner = actor;
                    selected.request = fresh_request;
                    selected.live = preserve;
                }
                break;
            }
            case 2:
                Fuzzing::dispatch(client, ReleaseRequestTransferLease { selected.key.source_client_id, selected.key.source_request_id });
                if (selected.live && selected.owner == actor)
                    selected.live = false;
                break;
            case 3:
                if (selected.live) {
                    auto reply = Fuzzing::dispatch(client, StopRequest { selected.request });
                    VERIFY(reply.success() == (selected.owner == actor));
                    if (selected.owner == actor)
                        selected.live = false;
                }
                break;
            case 4:
                for (auto& entry : model) {
                    if (entry.owner == actor)
                        entry.live = false;
                }
                replace_client(actor);
                break;
            }
        }
        size_t live_count = 0;
        for (auto const& entry : model) {
            if (!entry.live)
                continue;
            ++live_count;
            auto actual = leases.get(entry.key);
            VERIFY(actual.has_value());
            VERIFY(actual->owner.ptr() == clients[entry.owner].ptr());
            VERIFY(actual->request_id == entry.request);
        }
        VERIFY(leases.size() == live_count);
        // Positive control on both live receivers, including the unrelated peer.
        for (auto const& current : clients)
            VERIFY(Fuzzing::dispatch(*current, GetClientId {}).client_id() == current->client_id());
        if (input.byte() & 1)
            loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }
    // A transferred request can retain an older connection's curl handle. Stop
    // all created request IDs, including requests whose lease was consumed, before
    // releasing the fixture. Weak tracking does not prolong normal input lifetimes.
    for (auto& weak : generations) {
        if (auto client = weak.strong_ref()) {
            for (u64 request = 1; request <= 48; ++request)
                (void)Fuzzing::dispatch(*client, StopRequest { request });
            client->shutdown();
        }
    }
    VERIFY(leases.is_empty() && connections.is_empty());
    for (auto& client : clients)
        client = nullptr;
    for (auto& peer : peers)
        peer = nullptr;
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    return 0;
}
