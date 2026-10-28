/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ConditionVariable.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <AK/ThreadID.h>
#include <AK/ThreadSafeWeakable.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibMedia/RemoteVideoDecoder.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibMediaCodecs/VideoDecoderClientEndpoint.h>
#include <LibMediaCodecs/VideoDecoderServerEndpoint.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoFrameMetadata.h>
#include <LibMediaCodecs/VideoFramePool.h>
#include <LibThreading/Thread.h>

namespace Media {

static constexpr size_t MINIMUM_RING_SIZE = 4 * MiB;
static constexpr size_t MAXIMUM_RING_SIZE = 256 * MiB;
static constexpr AK::Duration SERVER_SILENCE_UNTIL_TREATED_AS_LOST = AK::Duration::from_seconds(10);

namespace {

class VideoDecoderClientConnection;

struct EventLoopThread {
    Core::EventLoop* event_loop { nullptr };
    AK::ThreadID thread_id;
};

}

// Shared by a decoder with the connection that routes its server's answers to it.
struct RemoteVideoDecoder::State : public AtomicRefCounted<State> {
    struct Output {
        NonnullRefPtr<VideoFrame> frame;
    };

    // A coded frame the server has yet to answer, whose data occupies its range of the ring until then.
    struct SubmissionInFlight {
        u64 ring_serial { 0 };
        u64 offset { 0 };
        u64 size { 0 };
        u32 generation { 0 };
    };

    size_t submissions_in_flight_of_current_generation() const
    {
        size_t count = 0;
        for (auto const& submission : submissions_in_flight) {
            if (submission.generation == generation)
                count++;
        }
        return count;
    }

    // The outputs are returned to be dropped after the mutex is released, since releasing their frames takes it.
    [[nodiscard]] Vector<Output> forget_server_while_locked()
    {
        connection_was_lost = true;
        connection = {};
        auto forgotten_outputs = move(outputs);
        server_is_waiting_for_storage = false;
        submissions_in_flight.clear();
        failure.clear();
        server_decoder_state_changed.broadcast();
        drained = false;
        return forgotten_outputs;
    }

    Mutex mutex;
    ConditionVariable server_decoder_state_changed { mutex };
    ThreadSafeWeakRef<VideoDecoderClientConnection> connection;
    u64 decoder_id { 0 };
    u32 generation { 0 };
    bool connection_was_lost { false };
    // The server's answers are not wanted anymore, though the frames the decoder handed out still go back to it.
    bool decoder_was_destroyed { false };

    Vector<Output> outputs;
    Vector<SubmissionInFlight> submissions_in_flight;
    Optional<DecoderError> failure;
    bool drained { false };

    // Called under the mutex when the server reports something the decoder's caller is not already waiting for, so
    // that destroying the decoder, which clears it, ends its calls.
    Function<void()> output_arrived_callback;
    bool caller_is_waiting_for_output { false };

    // Set once before any frame is submitted, and called after the mutex is released.
    Function<void()> storage_freed_callback;
    // Set while the server waits for a frame to be released, which our caller is then answered TryAgain for. The server
    // reports nothing else while it waits, so anything it reports for the generation it waited in ends the wait too.
    bool server_is_waiting_for_storage { false };

    // Returns whether the server was waiting for storage, so that the storage freed callback must be called.
    [[nodiscard]] bool clear_server_wait_for_storage_while_locked()
    {
        return exchange(server_is_waiting_for_storage, false);
    }

    // Returns whether the server was waiting for this release, so that the storage freed callback must be called.
    [[nodiscard]] bool release_frame_to_server_while_locked(u64 frame_id)
    {
        connection.with_target([&](auto& connection) {
            connection.async_release_frame(frame_id);
        });
        return clear_server_wait_for_storage_while_locked();
    }

    void tell_caller_of_new_state_while_locked()
    {
        if (!caller_is_waiting_for_output && output_arrived_callback)
            output_arrived_callback();
    }
};

namespace {

NeverDestroyed<RemoteVideoDecoder::TransportRequester> s_transport_requester;
Core::EventLoop* s_transport_requester_event_loop { nullptr };
AK::ThreadID s_transport_requester_thread_id;

// A request for a transport is in flight until its answer has been turned into a connection, or found to be none.
NeverDestroyed<Mutex> s_transport_request_mutex;
NeverDestroyed<ConditionVariable> s_transport_request_condition { *s_transport_request_mutex };
bool s_transport_request_is_in_flight { false };
u64 s_transport_requests_answered { 0 };

NeverDestroyed<Mutex> s_capabilities_mutex;
NeverDestroyed<HashMap<ParsedCodec, Optional<DecoderCapabilities>>> s_capabilities_by_format;

Atomic<u64> s_next_decoder_id { 1 };

template<typename T>
T run_on_thread(EventLoopThread const& thread, Function<T()> function)
{
    if (thread.thread_id.is_current_thread())
        return function();

    IGNORE_USE_IN_ESCAPING_LAMBDA Mutex mutex;
    IGNORE_USE_IN_ESCAPING_LAMBDA ConditionVariable condition { mutex };
    IGNORE_USE_IN_ESCAPING_LAMBDA Optional<T> result;
    thread.event_loop->deferred_invoke([&, function = move(function)] {
        auto function_result = function();
        MutexLocker locker { mutex };
        result = move(function_result);
        condition.broadcast();
    });

    MutexLocker locker { mutex };
    while (!result.has_value())
        condition.wait();
    return result.release_value();
}

// The connection's answers are handled here rather than on a busy event loop, since each one can hold up a decode.
// This thread never waits on another.
EventLoopThread const& connection_thread()
{
    static EventLoopThread const connection_thread = [] {
        IGNORE_USE_IN_ESCAPING_LAMBDA Mutex mutex;
        IGNORE_USE_IN_ESCAPING_LAMBDA ConditionVariable condition { mutex };
        IGNORE_USE_IN_ESCAPING_LAMBDA Optional<EventLoopThread> started_thread;
        auto thread = MUST(Threading::Thread::try_create("VideoDecoderIPC"sv, [&]() -> intptr_t {
            Core::EventLoop event_loop;
            {
                MutexLocker locker { mutex };
                started_thread = EventLoopThread { &event_loop, AK::ThreadID::current() };
                condition.broadcast();
            }
            return event_loop.exec();
        }));
        thread->start();
        thread->detach();

        MutexLocker locker { mutex };
        while (!started_thread.has_value())
            condition.wait();
        return *started_thread;
    }();
    return connection_thread;
}

template<typename T>
T run_on_connection_thread(Function<T()> function)
{
    return run_on_thread(connection_thread(), move(function));
}

// The connection to the VideoDecoderServer, used only on the connection thread except through weak references, whose
// targets are reached under the reference's lock.
class VideoDecoderClientConnection final
    : public IPC::ConnectionToServer<VideoDecoderClientEndpoint, VideoDecoderServerEndpoint>
    , public VideoDecoderClientEndpoint
    , public ThreadSafeWeakable<VideoDecoderClientConnection> {
    C_OBJECT_ABSTRACT(VideoDecoderClientConnection);

public:
    static ErrorOr<ThreadSafeWeakRef<VideoDecoderClientConnection>> acquire()
    {
        VERIFY(!connection_thread().thread_id.is_current_thread());
        if (auto connection = current(); connection.has_value())
            return connection.release_value();

        if (!*s_transport_requester)
            return Error::from_string_literal("No video decoder server transport requester is installed");
        // The requester answers through its thread's event loop, which that thread could not return to while waiting.
        VERIFY(!s_transport_requester_thread_id.is_current_thread());

        MutexLocker locker { *s_transport_request_mutex };
        // A request answered since the check above has already connected.
        if (auto connection = current(); connection.has_value())
            return connection.release_value();
        auto requests_answered = s_transport_requests_answered;
        if (!s_transport_request_is_in_flight) {
            s_transport_request_is_in_flight = true;
            s_transport_requester_event_loop->deferred_invoke([] {
                (*s_transport_requester)(transport_answered);
            });
        }
        while (s_transport_requests_answered == requests_answered)
            s_transport_request_condition->wait();

        if (auto connection = current(); connection.has_value())
            return connection.release_value();
        return Error::from_string_literal("Could not connect to a video decoder server");
    }

    static void transport_answered(ErrorOr<NonnullOwnPtr<IPC::Transport>> transport)
    {
        VERIFY(s_transport_requester_thread_id.is_current_thread());
        if (transport.is_error()) {
            dbgln("Could not get a transport to a video decoder server: {}", transport.error());
        } else {
            run_on_connection_thread<ThreadSafeWeakRef<VideoDecoderClientConnection>>([transport = transport.release_value()]() mutable {
                *s_connection = adopt_ref(*new VideoDecoderClientConnection(move(transport)));
                auto connection = (*s_connection)->make_weak_ref();
                MutexLocker locker { *s_current_connection_mutex };
                *s_current_connection = connection;
                return connection;
            });
        }

        MutexLocker locker { *s_transport_request_mutex };
        s_transport_request_is_in_flight = false;
        s_transport_requests_answered++;
        s_transport_request_condition->broadcast();
    }

    // Shuts the connection down on its own thread, where its death is handled, for a caller that found the server
    // unresponsive from any other thread.
    static void disconnect_from_unresponsive_server(ThreadSafeWeakRef<VideoDecoderClientConnection> connection)
    {
        connection_thread().event_loop->deferred_invoke([connection = move(connection)] {
            // Shutting down revokes the weak reference, so it happens outside the reference's lock.
            RefPtr<VideoDecoderClientConnection> strong_connection;
            connection.with_target([&](auto& target) { strong_connection = target; });
            if (strong_connection)
                strong_connection->shutdown();
        });
    }

    static Optional<Optional<DecoderCapabilities>> query_capabilities(ParsedCodec const& codec)
    {
        auto connection = acquire();
        if (connection.is_error()) {
            dbgln("Could not reach the video decoder server: {}", connection.error());
            return {};
        }
        return run_on_connection_thread<Optional<Optional<DecoderCapabilities>>>([codec]() -> Optional<Optional<DecoderCapabilities>> {
            if (s_connection->is_null())
                return {};
            auto response = (*s_connection)->send_sync_but_allow_failure<Messages::VideoDecoderServer::DecoderCapabilities>(codec);
            if (!response)
                return {};
            return response->capabilities();
        });
    }

    virtual ~VideoDecoderClientConnection() override
    {
        revoke_weak_refs();
    }

    void register_decoder(u64 decoder_id, NonnullRefPtr<RemoteVideoDecoder::State> state)
    {
        MutexLocker locker { m_decoders_mutex };
        m_decoders.set(decoder_id, move(state));
    }

    void unregister_decoder(u64 decoder_id)
    {
        MutexLocker locker { m_decoders_mutex };
        m_decoders.remove(decoder_id);
    }

private:
    explicit VideoDecoderClientConnection(NonnullOwnPtr<IPC::Transport> transport)
        : IPC::ConnectionToServer<VideoDecoderClientEndpoint, VideoDecoderServerEndpoint>(*this, move(transport))
    {
    }

    static Optional<ThreadSafeWeakRef<VideoDecoderClientConnection>> current()
    {
        MutexLocker locker { *s_current_connection_mutex };
        return *s_current_connection;
    }

    // Only the connection thread uses this.
    static NeverDestroyed<RefPtr<VideoDecoderClientConnection>> s_connection;
    // The connection that is open, for other threads to reach.
    static NeverDestroyed<Mutex> s_current_connection_mutex;
    static NeverDestroyed<Optional<ThreadSafeWeakRef<VideoDecoderClientConnection>>> s_current_connection;

    virtual void die() override
    {
        // NB: Forgetting this connection below can drop the last reference to it.
        NonnullRefPtr keep_alive_until_death_is_handled = *this;
        revoke_weak_refs();
        if (*s_connection == this) {
            *s_connection = nullptr;
            MutexLocker locker { *s_current_connection_mutex };
            *s_current_connection = {};
        }

        HashMap<u64, NonnullRefPtr<RemoteVideoDecoder::State>> decoders;
        {
            MutexLocker locker { m_decoders_mutex };
            decoders = move(m_decoders);
        }
        for (auto& [decoder_id, state] : decoders) {
            Vector<RemoteVideoDecoder::State::Output> forgotten_outputs;
            MutexLocker locker { state->mutex };
            if (state->decoder_id != decoder_id)
                continue;
            forgotten_outputs = state->forget_server_while_locked();
            state->tell_caller_of_new_state_while_locked();
        }
    }

    RefPtr<RemoteVideoDecoder::State> find_decoder(u64 decoder_id)
    {
        MutexLocker locker { m_decoders_mutex };
        if (auto state = m_decoders.get(decoder_id); state.has_value())
            return *state;
        return nullptr;
    }

    // What the decoder's caller is told of once the mutex is released: new state to take, through the output arrived
    // callback unless it is already waiting, and the end of a wait for storage it was answered TryAgain for, through
    // the storage freed callback.
    struct CallerWakeup {
        bool has_something_to_take { false };
        bool storage_was_freed { false };
    };

    // Returns whether the server reported for a decoder that is still known.
    template<typename Callback>
    bool with_decoder(u64 decoder_id, Callback callback)
    {
        auto state = find_decoder(decoder_id);
        if (!state)
            return false;
        CallerWakeup wakeup;
        {
            MutexLocker locker { state->mutex };
            if (state->decoder_id != decoder_id || state->decoder_was_destroyed)
                return false;
            wakeup = callback(*state);
            state->server_decoder_state_changed.broadcast();
            if (wakeup.has_something_to_take)
                state->tell_caller_of_new_state_while_locked();
        }
        if (wakeup.storage_was_freed && state->storage_freed_callback)
            state->storage_freed_callback();
        return true;
    }

    // A server that breaks the protocol is treated as lost, so that its decoders start over with a new one.
    void disconnect_misbehaving_server(StringView reason, Error const& error)
    {
        dbgln("The video decoder server misbehaved ({}: {}), disconnecting.", reason, error);
        shutdown();
    }

    virtual void frame_decoded(u64 decoder_id, u32 generation, u64 frame_id, VideoFrameMetadata metadata, VideoFramePixels pixels) override
    {
        auto state = find_decoder(decoder_id);
        if (!state) {
            async_release_frame(frame_id);
            return;
        }

        // A frame can outlive the server it came from, whose replacement must not hear of its release.
        auto release_to_server = [state, decoder_id, frame_id] {
            auto server_was_waiting_for_storage = false;
            {
                MutexLocker locker { state->mutex };
                if (state->decoder_id != decoder_id)
                    return;
                server_was_waiting_for_storage = state->release_frame_to_server_while_locked(frame_id);
            }
            if (server_was_waiting_for_storage && state->storage_freed_callback)
                state->storage_freed_callback();
        };
        auto frame_or_error = create_received_video_frame(metadata, move(pixels), 0, move(release_to_server));
        if (frame_or_error.is_error()) {
            disconnect_misbehaving_server("unusable frame"sv, frame_or_error.release_error());
            return;
        }

        // A frame nobody takes is dropped after the mutex is released, since releasing it takes the mutex.
        RefPtr<VideoFrame> frame = frame_or_error.release_value();
        with_decoder(decoder_id, [&](RemoteVideoDecoder::State& state) -> CallerWakeup {
            if (generation != state.generation)
                return {};
            auto storage_was_freed = state.clear_server_wait_for_storage_while_locked();
            state.outputs.append({ frame.release_nonnull() });
            return { .has_something_to_take = true, .storage_was_freed = storage_was_freed };
        });
    }

    virtual void decoder_waiting_for_storage(u64 decoder_id, u32 generation) override
    {
        with_decoder(decoder_id, [&](RemoteVideoDecoder::State& state) -> CallerWakeup {
            if (generation == state.generation)
                state.server_is_waiting_for_storage = true;
            return {};
        });
    }

    virtual void coded_frame_consumed(u64 decoder_id) override
    {
        with_decoder(decoder_id, [&](RemoteVideoDecoder::State& state) -> CallerWakeup {
            if (!state.submissions_in_flight.is_empty())
                state.submissions_in_flight.remove(0);
            return {};
        });
    }

    virtual void decoder_failed(u64 decoder_id, u32 generation, DecoderError error) override
    {
        with_decoder(decoder_id, [&](RemoteVideoDecoder::State& state) -> CallerWakeup {
            if (generation != state.generation)
                return {};
            auto storage_was_freed = state.clear_server_wait_for_storage_while_locked();
            if (!state.failure.has_value())
                state.failure = move(error);
            return { .has_something_to_take = true, .storage_was_freed = storage_was_freed };
        });
    }

    virtual void decoder_drained(u64 decoder_id, u32 generation) override
    {
        with_decoder(decoder_id, [&](RemoteVideoDecoder::State& state) -> CallerWakeup {
            if (generation != state.generation)
                return {};
            auto storage_was_freed = state.clear_server_wait_for_storage_while_locked();
            state.drained = true;
            return { .has_something_to_take = true, .storage_was_freed = storage_was_freed };
        });
    }

    Mutex m_decoders_mutex;
    HashMap<u64, NonnullRefPtr<RemoteVideoDecoder::State>> m_decoders;
};

NeverDestroyed<RefPtr<VideoDecoderClientConnection>> VideoDecoderClientConnection::s_connection;
NeverDestroyed<Mutex> VideoDecoderClientConnection::s_current_connection_mutex;
NeverDestroyed<Optional<ThreadSafeWeakRef<VideoDecoderClientConnection>>> VideoDecoderClientConnection::s_current_connection;

}

void RemoteVideoDecoder::set_transport_requester(TransportRequester requester)
{
    *s_transport_requester = move(requester);
    s_transport_requester_event_loop = &Core::EventLoop::current();
    s_transport_requester_thread_id = AK::ThreadID::current();
}

Optional<DecoderCapabilities> RemoteVideoDecoder::capabilities(ParsedCodec const& codec)
{
    if (track_type_from_codec_id(codec.codec_id()) != TrackType::Video || !*s_transport_requester)
        return {};

    auto support_key = normalize_parsed_codec_for_support_keying(codec);
    {
        MutexLocker locker { *s_capabilities_mutex };
        if (auto cached_capabilities = s_capabilities_by_format->get(support_key); cached_capabilities.has_value())
            return *cached_capabilities;
    }

    auto capabilities = VideoDecoderClientConnection::query_capabilities(support_key);
    // An unanswered query is asked again next time, in case the server can be reached by then.
    if (!capabilities.has_value())
        return {};

    MutexLocker locker { *s_capabilities_mutex };
    s_capabilities_by_format->set(support_key, *capabilities);
    return *capabilities;
}

DecoderErrorOr<NonnullOwnPtr<RemoteVideoDecoder>> RemoteVideoDecoder::try_create(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    auto codec_configuration = DECODER_TRY_ALLOC(ByteBuffer::copy(codec_initialization_data));
    auto state = DECODER_TRY_ALLOC(try_make_ref_counted<State>());
    auto decoder = DECODER_TRY_ALLOC(adopt_nonnull_own_or_enomem(new (nothrow) RemoteVideoDecoder(codec_id, move(codec_configuration), move(state))));
    TRY(decoder->start_server_decoder());
    return decoder;
}

RemoteVideoDecoder::RemoteVideoDecoder(CodecID codec_id, ByteBuffer codec_configuration, NonnullRefPtr<State> state)
    : m_codec_id(codec_id)
    , m_codec_configuration(move(codec_configuration))
    , m_state(move(state))
{
}

RemoteVideoDecoder::~RemoteVideoDecoder()
{
    ThreadSafeWeakRef<VideoDecoderClientConnection> connection;
    u64 decoder_id = 0;
    Vector<State::Output> outputs;
    {
        MutexLocker locker { m_state->mutex };
        connection = m_state->connection;
        decoder_id = m_state->decoder_id;
        outputs = move(m_state->outputs);
        m_state->decoder_was_destroyed = true;
        m_state->output_arrived_callback = nullptr;
    }
    connection.with_target([&](auto& connection) {
        connection.async_destroy_decoder(decoder_id);
        connection.unregister_decoder(decoder_id);
    });
}

DecoderErrorOr<void> RemoteVideoDecoder::start_server_decoder()
{
    if (!*s_transport_requester)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "This process cannot reach a video decoder server"sv);
    if (m_server_loss_count > MAXIMUM_SERVER_LOSSES)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "The video decoder server was lost too many times"sv);

    auto connection_or_error = VideoDecoderClientConnection::acquire();
    if (connection_or_error.is_error())
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Could not reach the video decoder server: {}", connection_or_error.error());
    auto connection = connection_or_error.release_value();

    auto decoder_id = s_next_decoder_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
    u32 generation = 0;
    Vector<State::Output> forgotten_outputs;
    {
        MutexLocker locker { m_state->mutex };
        forgotten_outputs = m_state->forget_server_while_locked();
        m_state->connection_was_lost = false;
        m_state->connection = connection;
        m_state->decoder_id = decoder_id;
        generation = m_state->generation;
    }
    m_ring = {};
    m_server_loss_was_reported = false;

    auto connection_is_open = false;
    connection.with_target([&](auto& connection) {
        connection.register_decoder(decoder_id, m_state);
        connection.async_create_decoder(decoder_id, generation, m_codec_id, m_codec_configuration);
        connection_is_open = true;
    });
    if (!connection_is_open) {
        Vector<State::Output> outputs_of_the_unopened_connection;
        MutexLocker locker { m_state->mutex };
        outputs_of_the_unopened_connection = m_state->forget_server_while_locked();
    }
    return {};
}

void RemoteVideoDecoder::set_storage_freed_callback(Function<void()> callback)
{
    MutexLocker locker { m_state->mutex };
    m_state->storage_freed_callback = move(callback);
}

void RemoteVideoDecoder::set_output_arrived_callback(Function<void()> callback)
{
    MutexLocker locker { m_state->mutex };
    m_state->output_arrived_callback = move(callback);
}

DecoderError RemoteVideoDecoder::report_server_loss()
{
    m_server_loss_was_reported = true;
    m_server_loss_count++;
    return DecoderError::with_description(DecoderErrorCategory::NeedsResubmission, "The video decoder server was lost"sv);
}

DecoderErrorOr<u64> RemoteVideoDecoder::write_to_ring(ReadonlyBytes data)
{
    auto range_is_free = [&](u64 offset) {
        if (offset + data.size() > m_ring.size())
            return false;
        for (auto const& submission : m_state->submissions_in_flight) {
            if (submission.ring_serial != m_ring_serial)
                continue;
            if (offset < submission.offset + submission.size && submission.offset < offset + data.size())
                return false;
        }
        return true;
    };

    Optional<u64> offset;
    {
        MutexLocker locker { m_state->mutex };
        if (m_ring.is_valid()) {
            if (range_is_free(m_ring_write_offset))
                offset = m_ring_write_offset;
            else if (range_is_free(0))
                offset = 0;
        }
    }

    if (!offset.has_value()) {
        // The frames still in the old ring keep it alive on the server until it has consumed them.
        auto ring_size = max(m_ring.size() * 2, MINIMUM_RING_SIZE);
        while (ring_size < data.size() * 4 && ring_size < MAXIMUM_RING_SIZE)
            ring_size *= 2;
        if (data.size() > ring_size)
            return DecoderError::with_description(DecoderErrorCategory::Memory, "A coded video frame is too large to submit"sv);
        m_ring = DECODER_TRY_ALLOC(Core::AnonymousBuffer::create_with_size(ring_size));
        m_ring_serial++;
        offset = 0;

        MutexLocker locker { m_state->mutex };
        m_state->connection.with_target([&](auto& connection) {
            connection.async_replace_coded_data_ring(m_state->decoder_id, m_ring);
        });
    }

    data.copy_to(Bytes { m_ring.data<u8>() + *offset, data.size() });
    m_ring_write_offset = *offset + data.size();
    return *offset;
}

DecoderErrorOr<void> RemoteVideoDecoder::receive_coded_data(CodedFrame const& coded_frame, DecodeIntent intent)
{
    if (m_reached_end_of_stream)
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "The decoder has been drained"sv);

    {
        MutexLocker locker { m_state->mutex };
        if (m_state->connection_was_lost && !m_server_loss_was_reported)
            return report_server_loss();
    }
    if (m_server_loss_was_reported)
        TRY(start_server_decoder());

    if (auto new_codec_configuration = coded_frame.new_codec_configuration(); new_codec_configuration.has_value())
        m_codec_configuration = DECODER_TRY_ALLOC(ByteBuffer::copy(*new_codec_configuration));

    auto ring_offset = TRY(write_to_ring(coded_frame.data()));

    Optional<ByteBuffer> new_codec_configuration;
    if (coded_frame.new_codec_configuration().has_value())
        new_codec_configuration = m_codec_configuration;

    MutexLocker locker { m_state->mutex };
    m_state->submissions_in_flight.append({ m_ring_serial, ring_offset, coded_frame.data().size(), m_state->generation });
    m_state->connection.with_target([&](auto& connection) {
        connection.async_submit_coded_frame(m_state->decoder_id, m_state->generation, ring_offset, coded_frame.data().size(), coded_frame.presentation_timestamp(), coded_frame.decode_timestamp(), coded_frame.duration(), coded_frame.flags(), move(new_codec_configuration), intent, m_output_target);
    });
    return {};
}

void RemoteVideoDecoder::signal_end_of_stream()
{
    m_reached_end_of_stream = true;
    MutexLocker locker { m_state->mutex };
    m_state->drained = false;
    m_state->connection.with_target([&](auto& connection) {
        connection.async_signal_end_of_stream(m_state->decoder_id, m_state->generation);
    });
}

DecoderErrorOr<NonnullRefPtr<VideoFrame>> RemoteVideoDecoder::take_next_output(CodingIndependentCodePoints const& container_cicp, Optional<AK::Duration> target)
{
    // The server hears of the target with the next submission, which follows the flush that any new target comes after.
    m_output_target = target;

    Optional<State::Output> output;
    {
        MutexLocker locker { m_state->mutex };
        while (true) {
            if (m_state->connection_was_lost) {
                if (!m_server_loss_was_reported)
                    return report_server_loss();
                // Nothing is decoding until the frames are submitted again.
                if (m_reached_end_of_stream)
                    return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "The decoder has been drained"sv);
                return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "The decoder needs its frames submitted again"sv);
            }

            if (!m_state->outputs.is_empty()) {
                output = m_state->outputs.take_first();
                break;
            }
            if (m_state->failure.has_value())
                return m_state->failure.release_value();
            if (m_reached_end_of_stream) {
                if (m_state->drained)
                    return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "The decoder has been drained"sv);
            } else if (m_state->submissions_in_flight_of_current_generation() < MAXIMUM_FRAMES_IN_FLIGHT) {
                return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "The video decoder server has no frame ready"sv);
            }
            // Its answer waits on a slot that only our caller's consumers can release, however long they take.
            if (m_state->server_is_waiting_for_storage)
                return DecoderError::with_description(DecoderErrorCategory::TryAgain, "Every slot of the video decoder server is held"sv);
            m_state->caller_is_waiting_for_output = true;
            auto server_answered = m_state->server_decoder_state_changed.wait_for(SERVER_SILENCE_UNTIL_TREATED_AS_LOST);
            m_state->caller_is_waiting_for_output = false;
            if (!server_answered) {
                dbgln("The video decoder server has answered nothing for {} seconds, disconnecting.", SERVER_SILENCE_UNTIL_TREATED_AS_LOST.to_seconds());
                VideoDecoderClientConnection::disconnect_from_unresponsive_server(m_state->connection);
            }
        }
    }

    auto const& frame = output->frame;
    auto cicp = container_cicp;
    cicp.adopt_specified_values(frame->cicp());
    return DECODER_TRY_ALLOC(try_make_ref_counted<VideoFrame>(
        frame->timestamp(), frame->duration(), frame->size(),
        frame->bit_depth(), frame->subsampling(), cicp, frame->storage()));
}

void RemoteVideoDecoder::flush()
{
    m_reached_end_of_stream = false;
    m_output_target = {};

    Vector<State::Output> outputs;
    MutexLocker locker { m_state->mutex };
    m_state->generation++;
    outputs = move(m_state->outputs);
    m_state->failure.clear();
    m_state->drained = false;
    m_state->server_is_waiting_for_storage = false;
    m_state->connection.with_target([&](auto& connection) {
        connection.async_flush(m_state->decoder_id, m_state->generation);
    });
}

}
