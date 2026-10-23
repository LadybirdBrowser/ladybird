/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <LibCore/EventLoop.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Fetching {

class BodyStreamPullSource;

// Never reused within a thread.
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, BodyStreamWakeToken);

// Delivers a channel's wakes to the pull source registered under its token, on the loop that created it.
class BodyStreamWakeSink final : public Engine::FetchByteChannelConsumerSink {
public:
    static NonnullRefPtr<BodyStreamWakeSink> create();

    BodyStreamWakeToken registry_token() const { return m_registry_token; }

    virtual void wake(Engine::WakeTicket) override;

private:
    explicit BodyStreamWakeSink(BodyStreamWakeToken);

    NonnullRefPtr<Core::WeakEventLoopReference> m_loop;
    BodyStreamWakeToken const m_registry_token { 0 };
};

// The pull algorithm of a response stream whose bytes arrive through a FetchByteChannel.
class BodyStreamPullSource final : public JS::Cell {
    GC_CELL(BodyStreamPullSource, JS::Cell);
    GC_DECLARE_ALLOCATOR(BodyStreamPullSource);

public:
    BodyStreamPullSource(JS::Realm&, GC::Ref<Infrastructure::FetchParams const>, GC::Ref<Streams::ReadableStream>, NonnullRefPtr<Engine::FetchByteChannel>, BodyStreamWakeSink const&);
    virtual ~BodyStreamPullSource() override;

    GC::Ref<WebIDL::Promise> pull();
    GC::Ref<WebIDL::Promise> cancel(JS::Value reason);

    enum class WakeResult : u8 {
        Consumed,
        StillAwaited,
    };
    WakeResult on_wake(Engine::WakeTicket);
    void handle_network_settled();
    void deliver_while_paused();

private:
    virtual void visit_edges(Visitor& visitor) override;

    void settle_stream(Engine::FetchByteChannel::TakeResult const&);
    void run_pull_task();
    void drain_into_stream();

    // The realm the stream is set up in.
    GC::Ref<JS::Realm> m_realm;
    GC::Ref<Infrastructure::FetchParams const> m_fetch_params;
    GC::Ref<Streams::ReadableStream> m_stream;
    NonnullRefPtr<Engine::FetchByteChannel> m_channel;
    BodyStreamWakeToken const m_registry_token { 0 };

    GC::Ptr<WebIDL::Promise> m_pending_pull_promise;
    Engine::WakeTicket m_expected_ticket { 0 };

    // Once the channel has ended, later pulls stay pending.
    bool m_detached { false };
};

}
