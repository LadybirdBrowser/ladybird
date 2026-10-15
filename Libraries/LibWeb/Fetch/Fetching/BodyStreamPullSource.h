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
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
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

// The pull algorithm of a response stream whose bytes arrive through a FetchByteChannel. The channel's end reaches the
// stream even when no pull is pending.
class BodyStreamPullSource final : public JS::Cell {
    GC_CELL(BodyStreamPullSource, JS::Cell);
    GC_DECLARE_ALLOCATOR(BodyStreamPullSource);

public:
    BodyStreamPullSource(JS::Realm&, GC::Ptr<Infrastructure::FetchController>, GC::Ref<Streams::ReadableStream>, NonnullRefPtr<Engine::FetchByteChannel>, BodyStreamWakeSink const&);
    virtual ~BodyStreamPullSource() override;

    GC::Ref<WebIDL::Promise> pull();
    GC::Ref<WebIDL::Promise> cancel(JS::Value reason);

    enum class WakeResult : u8 {
        Consumed,
        StillAwaited,
    };
    WakeResult on_wake(Engine::WakeTicket);

    // Waits for the channel to end, and while the HTML event loop is paused for its bytes too.
    void watch_channel();

    // Reads the resource header once the channel buffers it or ends, leaving its bytes in the channel. It is read
    // before any byte is taken from the channel.
    void read_resource_header(Infrastructure::Body::ResourceHeaderCallback);

private:
    virtual void visit_edges(Visitor& visitor) override;

    Engine::WakeTicket arm_waiter(Engine::FetchByteChannel::WakeOn);
    void on_pull_wake();
    void push_while_paused();
    void on_push_wake();
    void on_end_wake();
    void on_resource_header_wake(Engine::WakeTicket);
    void settle_without_reading();
    void settle_stream(Engine::FetchByteChannel::TakeResult const&);
    void run_pull_task();
    void drain_into_stream();

    // The realm the stream is set up in.
    GC::Ref<JS::Realm> m_realm;
    // Null for a navigation body adopted from another process.
    GC::Ptr<Infrastructure::FetchController> m_fetch_controller;
    GC::Ref<Streams::ReadableStream> m_stream;
    NonnullRefPtr<Engine::FetchByteChannel> m_channel;
    BodyStreamWakeToken const m_registry_token { 0 };

    GC::Ptr<WebIDL::Promise> m_pending_pull_promise;
    Optional<Engine::WakeTicket> m_end_ticket;
    Optional<Engine::WakeTicket> m_pull_ticket;
    Optional<Engine::WakeTicket> m_push_ticket;
    Optional<Engine::WakeTicket> m_resource_header_ticket;
    GC::Ptr<GC::Function<void(Core::ImmutableBytes, Infrastructure::Body::SniffOutcome)>> m_on_resource_header;
    bool m_has_taken_bytes { false };

    // Once the channel has ended, later pulls stay pending.
    bool m_detached { false };
};

// The view of a body whose bytes arrive through the channel; its stream's pull source is the channel's consumer.
WEB_API GC::Ref<Infrastructure::Body> create_body_from_channel(JS::Realm&, GC::Ptr<Infrastructure::FetchController>, NonnullRefPtr<Engine::FetchByteChannel>);

}
