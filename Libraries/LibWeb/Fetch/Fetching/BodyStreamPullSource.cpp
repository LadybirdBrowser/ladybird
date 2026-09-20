/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/Error.h>
#include <LibWeb/Fetch/Fetching/BodyStreamPullSource.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Fetch::Fetching {

GC_DEFINE_ALLOCATOR(BodyStreamPullSource);

// Roots a pull source only while a wake is owed; tokens are never reused, and the consumer loop must outlive every
// source with a wake owed.
class BodyStreamWakeRegistry {
public:
    static BodyStreamWakeRegistry& the()
    {
        static thread_local NeverDestroyed<BodyStreamWakeRegistry> registry;
        return *registry;
    }

    BodyStreamWakeToken allocate_token() { return BodyStreamWakeToken { m_next_token++ }; }

    void arm(BodyStreamWakeToken token, BodyStreamPullSource& source)
    {
        m_armed_sources.ensure(token, [&] { return GC::make_root(source); });
    }
    void disarm(BodyStreamWakeToken token) { m_armed_sources.remove(token); }

    void notify(BodyStreamWakeToken token, Engine::WakeTicket ticket)
    {
        auto source = m_armed_sources.take(token);
        if (!source.has_value())
            return;
        if ((*source)->on_wake(ticket) == BodyStreamPullSource::WakeResult::StillAwaited)
            m_armed_sources.set(token, source.release_value());
    }

private:
    u64 m_next_token { 1 };
    HashMap<BodyStreamWakeToken, GC::Root<BodyStreamPullSource>> m_armed_sources;
};

NonnullRefPtr<BodyStreamWakeSink> BodyStreamWakeSink::create()
{
    return adopt_ref(*new BodyStreamWakeSink(BodyStreamWakeRegistry::the().allocate_token()));
}

BodyStreamWakeSink::BodyStreamWakeSink(BodyStreamWakeToken registry_token)
    : m_loop(Core::EventLoop::current_weak())
    , m_registry_token(registry_token)
{
}

void BodyStreamWakeSink::wake(Engine::WakeTicket ticket)
{
    auto loop = m_loop->take();
    if (!loop)
        return;
    loop->deferred_invoke([token = m_registry_token, ticket] {
        BodyStreamWakeRegistry::the().notify(token, ticket);
    });
}

BodyStreamPullSource::BodyStreamPullSource(JS::Realm& realm, GC::Ref<Infrastructure::FetchParams const> fetch_params, GC::Ref<Streams::ReadableStream> stream, NonnullRefPtr<Engine::FetchByteChannel> channel, BodyStreamWakeSink const& wake_sink)
    : m_realm(realm)
    , m_fetch_params(fetch_params)
    , m_stream(stream)
    , m_channel(move(channel))
    , m_registry_token(wake_sink.registry_token())
{
}

BodyStreamPullSource::~BodyStreamPullSource() = default;

void BodyStreamPullSource::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
    visitor.visit(m_fetch_params);
    visitor.visit(m_stream);
    visitor.visit(m_pending_pull_promise);
}

// This implements the pullAlgorithm of HTTP-network fetch.
// https://fetch.spec.whatwg.org/#concept-http-network-fetch
GC::Ref<WebIDL::Promise> BodyStreamPullSource::pull()
{
    // 1. Let promise be a new promise.
    auto promise = WebIDL::create_promise(*m_realm);

    // 2. Run the following steps in parallel:
    //    1. If the size of buffer is smaller than a lower limit chosen by the user agent and the
    //       ongoing fetch is suspended, resume the fetch.
    //    2. Wait until buffer is not empty.
    VERIFY(!m_pending_pull_promise);
    m_pending_pull_promise = promise;
    if (!m_detached) {
        BodyStreamWakeRegistry::the().arm(m_registry_token, *this);
        m_expected_ticket = m_channel->arm_waiter();
    }

    // 3. Return promise.
    return promise;
}

GC::Ref<WebIDL::Promise> BodyStreamPullSource::cancel(JS::Value reason)
{
    auto& realm = *m_realm;
    m_channel->cancel();
    BodyStreamWakeRegistry::the().disarm(m_registry_token);
    m_detached = true;

    // A pull promise created before the cancellation still settles.
    if (auto promise = exchange(m_pending_pull_promise, nullptr))
        WebIDL::resolve_promise(realm, *promise, JS::js_undefined());

    // cancelAlgorithm: abort fetchParams's controller with reason, given reason.
    m_fetch_params->controller()->abort(realm, reason);
    return WebIDL::create_resolved_promise(realm, JS::js_undefined());
}

BodyStreamPullSource::WakeResult BodyStreamPullSource::on_wake(Engine::WakeTicket ticket)
{
    if (!m_pending_pull_promise)
        return WakeResult::Consumed;
    // This waiter's wake is still owed; the source stays rooted for it.
    if (ticket != m_expected_ticket)
        return WakeResult::StillAwaited;

    // AD-HOC: While a sync XHR send() pauses the HTML event loop, the pull runs now rather than in a fetch task.
    if (HTML::main_thread_event_loop().execution_paused()) {
        run_pull_task();
        return WakeResult::Consumed;
    }

    // 3. Queue a fetch task to run the following steps, with fetchParams’s task destination:
    // NB: This task stays queued when the fetch is stopped.
    Infrastructure::queue_fetch_task(m_fetch_params->task_destination(), GC::create_function(heap(), [this] {
        run_pull_task();
    }));
    return WakeResult::Consumed;
}

// A failed transmission errors the stream regardless of read demand, and an empty completed body closes without one.
void BodyStreamPullSource::handle_network_settled()
{
    if (m_pending_pull_promise)
        return;

    if (HTML::main_thread_event_loop().execution_paused()) {
        deliver_while_paused();
        return;
    }

    auto& realm = *m_realm;
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    settle_stream(m_channel->take_up_to(0));
}

// This implements the ending of the in-parallel steps of HTTP-network fetch.
// https://fetch.spec.whatwg.org/#concept-http-network-fetch
void BodyStreamPullSource::settle_stream(Engine::FetchByteChannel::TakeResult const& result)
{
    result.state.visit(
        [](Engine::FetchByteChannel::Open) { },
        [](Engine::FetchByteChannel::ConsumerCancelled) { },
        [&](Engine::FetchByteChannel::Closed) {
            // 2. Otherwise, if the bytes transmission for response’s message body is done normally and stream is
            //    readable, then close stream, and abort these in-parallel steps.
            // AD-HOC: Closing stream while buffer still holds bytes would lose them, so stream closes once they have
            //         been pulled into it.
            if (result.buffered_byte_count == 0 && m_stream->is_readable()) {
                m_stream->close();
                m_detached = true;
            }
        },
        [&](Engine::FetchByteChannel::Errored const& errored) {
            // 2. Otherwise, if stream is readable, error stream with a TypeError.
            if (m_stream->is_readable()) {
                auto message = Utf16String::from_utf8(StringView { errored.message });
                m_stream->error(JS::TypeError::create(*m_realm, message));
            }
            m_detached = true;
        });
}

// AD-HOC: A sync XHR send() pauses the HTML event loop while it waits for exactly these bytes, and a stream set up
//         under that pause never starts pulling: the start promise's reaction is a microtask. So while the loop is
//         paused, bytes are pushed into the stream as they arrive, the way the specification's in-parallel steps
//         would deliver them (see the sync XHR body path in fetch_response_handover()).
void BodyStreamPullSource::deliver_while_paused()
{
    // A pending pull's wake carries these bytes to run_pull_task() itself.
    if (m_detached || m_pending_pull_promise)
        return;

    drain_into_stream();
}

void BodyStreamPullSource::run_pull_task()
{
    auto promise = exchange(m_pending_pull_promise, nullptr);
    if (!promise)
        return;

    drain_into_stream();

    // 3. Resolve promise with undefined.
    auto& realm = *m_realm;
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    WebIDL::resolve_promise(realm, *promise, JS::js_undefined());
}

void BodyStreamPullSource::drain_into_stream()
{
    auto& realm = *m_realm;
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    // The slices removed by one take reach the stream as one byte sequence, and a BYOB pull may consume only the
    // current view's length.
    auto result = [&] {
        if (auto byte_length = m_stream->current_byob_request_view_byte_length(); byte_length.has_value())
            return m_channel->take_up_to(*byte_length);
        return m_channel->take_all();
    }();

    // 1. Pull from bytes buffer into stream.
    if (!result.taken.is_empty()) {
        ByteBuffer bytes;
        for (auto const& taken : result.taken)
            bytes.append(taken.bytes());
        if (auto pull_result = m_stream->pull_from_bytes(bytes); pull_result.is_error()) {
            // A failed enqueue ends the whole body.
            auto error = WebIDL::exception_to_throw_completion(realm.vm(), realm, pull_result.release_error());
            if (m_stream->is_readable())
                m_stream->error(error.value());
            m_fetch_params->controller()->terminate();
            m_channel->cancel();
            m_detached = true;
            return;
        }
    }

    // 2. If stream is errored, then terminate fetchParams’s controller.
    if (m_stream->is_errored())
        m_fetch_params->controller()->terminate();

    settle_stream(result);
    if (result.state.has<Engine::FetchByteChannel::ConsumerCancelled>())
        m_detached = true;
}

}
