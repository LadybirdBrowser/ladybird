/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/ConditionVariable.h>
#include <AK/Queue.h>
#include <AK/Variant.h>
#include <LibCore/EventLoop.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibMediaCodecs/VideoDecoder.h>
#include <LibMediaCodecs/VideoDecoderServerConnection.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoFramePool.h>
#include <LibThreading/Thread.h>

namespace Media {

class VideoDecoderServerConnection::DecoderSession final : public AtomicRefCounted<DecoderSession> {
public:
    struct Submission {
        u32 generation { 0 };
        u64 ring_offset { 0 };
        u64 size { 0 };
        AK::Duration presentation_timestamp;
        AK::Duration decode_timestamp;
        AK::Duration duration;
        FrameFlags flags { FrameFlags::None };
        Optional<ByteBuffer> new_codec_configuration;
        DecodeIntent intent { DecodeIntent::Output };
        Optional<AK::Duration> output_target;
    };
    struct EndOfStream {
        u32 generation { 0 };
    };
    struct Flush {
        u32 generation { 0 };
    };
    struct ReplaceRing {
        Core::AnonymousBuffer ring;
    };
    using Command = Variant<Submission, EndOfStream, Flush, ReplaceRing>;

    static ErrorOr<NonnullRefPtr<DecoderSession>> create(ThreadSafeWeakRef<VideoDecoderServerConnection> connection, Core::EventLoop& connection_event_loop, ReadonlySpan<VideoDecoderRegistration> hosted_decoders, u64 decoder_id, u32 generation, CodecID codec_id, ByteBuffer codec_initialization_data)
    {
        auto session = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) DecoderSession(move(connection), connection_event_loop, hosted_decoders, decoder_id, generation, codec_id, move(codec_initialization_data))));
        session->m_thread = TRY(Threading::Thread::try_create("Video Decoder"sv, [session]() -> intptr_t {
            session->run();
            return 0;
        }));
        session->m_thread->start();
        return session;
    }

    void enqueue(Command command)
    {
        MutexLocker locker { m_wait_state->mutex };
        if (auto const* flush = command.get_pointer<Flush>())
            m_newest_requested_generation = flush->generation;
        m_commands.enqueue(move(command));
        m_wait_state->condition.broadcast();
    }

    void exit_and_detach()
    {
        request_exit();
        m_thread->detach();
        m_thread = nullptr;
    }

    // Once this returns, the decoder will not write to any slot again.
    void exit_and_join()
    {
        request_exit();
        (void)m_thread->join();
        m_thread = nullptr;
    }

private:
    void request_exit()
    {
        MutexLocker locker { m_wait_state->mutex };
        m_exit_requested = true;
        m_wait_state->condition.broadcast();
    }

    // Shared with the decoder's callbacks, which frames held by the client can outlive the session with.
    struct WaitState : public AtomicRefCounted<WaitState> {
        Mutex mutex;
        ConditionVariable condition { mutex };
        bool storage_was_freed { false };
        bool output_arrived { false };
    };

    DecoderSession(ThreadSafeWeakRef<VideoDecoderServerConnection> connection, Core::EventLoop& connection_event_loop, ReadonlySpan<VideoDecoderRegistration> hosted_decoders, u64 decoder_id, u32 generation, CodecID codec_id, ByteBuffer codec_initialization_data)
        : m_connection(move(connection))
        , m_connection_event_loop(connection_event_loop)
        , m_hosted_decoders(hosted_decoders)
        , m_decoder_id(decoder_id)
        , m_codec_id(codec_id)
        , m_codec_initialization_data(move(codec_initialization_data))
        , m_newest_requested_generation(generation)
        , m_generation(generation)
    {
    }

    void run()
    {
        create_decoder();
        while (true) {
            Optional<Command> command;
            auto output_arrived = false;
            {
                MutexLocker locker { m_wait_state->mutex };
                while (m_commands.is_empty() && !m_wait_state->output_arrived && !m_exit_requested)
                    m_wait_state->condition.wait();
                if (m_exit_requested)
                    break;
                output_arrived = exchange(m_wait_state->output_arrived, false);
                if (!m_commands.is_empty())
                    command = m_commands.dequeue();
            }
            // Output the decoder finished on its own goes out before the next command, which it does not depend on.
            if (output_arrived && !m_failure.has_value())
                take_outputs();
            if (command.has_value()) {
                command->visit(
                    [&](Submission& submission) { decode_submission(submission); },
                    [&](EndOfStream const& end_of_stream) { drain_to_end_of_stream(end_of_stream.generation); },
                    [&](Flush const& flush) { apply_flush(flush.generation); },
                    [&](ReplaceRing& replace_ring) { m_ring = move(replace_ring.ring); });
            }
        }
        m_decoder.clear();
    }

    void create_decoder()
    {
        auto decoder_or_error = [&] -> DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> {
            for (auto const& registration : m_hosted_decoders) {
                auto decoder = registration.create(m_codec_id, m_codec_initialization_data);
                if (!decoder.is_error() || decoder.error().category() != DecoderErrorCategory::NotImplemented)
                    return decoder;
            }
            return DecoderError::format(DecoderErrorCategory::NotImplemented, "The video decoder server has no decoder for codec {}", m_codec_id);
        }();
        if (decoder_or_error.is_error()) {
            fail(decoder_or_error.release_error());
            return;
        }
        m_decoder = decoder_or_error.release_value();
        m_decoder->set_storage_freed_callback([wait_state = m_wait_state] {
            MutexLocker locker { wait_state->mutex };
            wait_state->storage_was_freed = true;
            wait_state->condition.broadcast();
        });
        m_decoder->set_output_arrived_callback([wait_state = m_wait_state] {
            MutexLocker locker { wait_state->mutex };
            wait_state->output_arrived = true;
            wait_state->condition.broadcast();
        });
    }

    bool is_superseded_by_flush(u32 generation)
    {
        MutexLocker locker { m_wait_state->mutex };
        return generation != m_newest_requested_generation;
    }

    void fail(DecoderError&& error)
    {
        m_failure = move(error);
        report_failure();
    }

    void report_failure()
    {
        VERIFY(m_failure.has_value());
        m_connection.with_target([&](auto& connection) {
            connection.async_decoder_failed(m_decoder_id, m_generation, *m_failure);
        });
    }

    void acknowledge_submission()
    {
        m_connection.with_target([&](auto& connection) {
            connection.async_coded_frame_consumed(m_decoder_id);
        });
    }

    DecoderErrorOr<CodedFrame> coded_frame_for_submission(Submission& submission)
    {
        Checked<u64> end = submission.ring_offset;
        end += submission.size;
        if (!m_ring.is_valid() || end.has_overflow() || end.value() > m_ring.size())
            return DecoderError::with_description(DecoderErrorCategory::Invalid, "A coded frame lies outside its ring"sv);

        auto data = DECODER_TRY_ALLOC(FixedArray<u8>::create(m_ring.bytes().slice(submission.ring_offset, submission.size)));
        Optional<FixedArray<u8>> new_codec_configuration;
        if (submission.new_codec_configuration.has_value())
            new_codec_configuration = DECODER_TRY_ALLOC(FixedArray<u8>::create(submission.new_codec_configuration->bytes()));
        return CodedFrame { m_codec_id, submission.presentation_timestamp, submission.decode_timestamp, submission.duration, submission.flags, move(data), move(new_codec_configuration) };
    }

    void decode_submission(Submission& submission)
    {
        if (m_failure.has_value() || is_superseded_by_flush(submission.generation)) {
            acknowledge_submission();
            return;
        }

        auto coded_frame_or_error = coded_frame_for_submission(submission);
        if (coded_frame_or_error.is_error()) {
            fail(coded_frame_or_error.release_error());
            acknowledge_submission();
            return;
        }

        auto receive_result = m_decoder->receive_coded_data(coded_frame_or_error.value(), submission.intent);
        if (receive_result.is_error()) {
            fail(receive_result.release_error());
        } else {
            m_output_target = submission.output_target;
            take_outputs();
        }
        acknowledge_submission();
    }

    void drain_to_end_of_stream(u32 generation)
    {
        if (m_failure.has_value() || is_superseded_by_flush(generation))
            return;
        m_decoder->signal_end_of_stream();
        m_output_target = {};
        if (!take_outputs())
            return;
        m_connection.with_target([&](auto& connection) {
            connection.async_decoder_drained(m_decoder_id, m_generation);
        });
    }

    void apply_flush(u32 generation)
    {
        m_generation = generation;
        m_output_target = {};
        if (m_failure.has_value()) {
            report_failure();
            return;
        }
        m_decoder->flush();
    }

    // Returns whether the decoder was drained or needs more input, rather than being abandoned or failing.
    bool take_outputs()
    {
        while (true) {
            auto frame_or_error = m_decoder->take_next_output({}, m_output_target);
            if (!frame_or_error.is_error()) {
                send_frame(frame_or_error.release_value());
                continue;
            }

            auto category = frame_or_error.error().category();
            if (category == DecoderErrorCategory::NeedsMoreInput || category == DecoderErrorCategory::EndOfStream)
                return true;
            if (category != DecoderErrorCategory::TryAgain) {
                fail(frame_or_error.release_error());
                return false;
            }

            // Every slot is held by the client, so wait for it to release one. A flush or exit means the frames
            // waiting for one are not wanted anymore.
            MutexLocker locker { m_wait_state->mutex };
            // The client's consumers may hold the slots indefinitely, so the client is told not to wait for an answer.
            if (!m_wait_state->storage_was_freed && !m_exit_requested && m_newest_requested_generation == m_generation) {
                m_connection.with_target([&](auto& connection) {
                    connection.async_decoder_waiting_for_storage(m_decoder_id, m_generation);
                });
            }
            while (!m_wait_state->storage_was_freed && !m_exit_requested && m_newest_requested_generation == m_generation)
                m_wait_state->condition.wait();
            m_wait_state->storage_was_freed = false;
            if (m_exit_requested || m_newest_requested_generation != m_generation)
                return false;
        }
    }

    void send_frame(NonnullRefPtr<VideoFrame> frame)
    {
        auto metadata = VideoFrameMetadata::for_frame(*frame);
        auto pixels = frame->storage()->pixels();

        auto should_close_connection = false;
        m_connection.with_target([&](auto& connection) {
            // The frame is held before the client hears of it, so that its release cannot arrive first.
            auto frame_id = connection.hold_frame_until_released(move(frame));
            connection.async_frame_decoded(m_decoder_id, m_generation, frame_id, metadata, move(pixels));
            should_close_connection = connection.should_close_after_decoding_frame();
        });
        if (should_close_connection)
            close_connection();
    }

    void close_connection()
    {
        m_connection_event_loop.deferred_invoke([weak_connection = m_connection] {
            // Shutting down revokes the weak reference, so it happens outside the reference's lock.
            RefPtr<VideoDecoderServerConnection> connection;
            weak_connection.with_target([&](auto& target) { connection = target; });
            if (connection)
                connection->shutdown();
        });
    }

    ThreadSafeWeakRef<VideoDecoderServerConnection> m_connection;
    Core::EventLoop& m_connection_event_loop;
    ReadonlySpan<VideoDecoderRegistration> const m_hosted_decoders;
    u64 const m_decoder_id;
    CodecID const m_codec_id;
    ByteBuffer const m_codec_initialization_data;

    // Only the connection's thread uses this.
    RefPtr<Threading::Thread> m_thread;

    NonnullRefPtr<WaitState> m_wait_state { make_ref_counted<WaitState>() };
    Queue<Command> m_commands;
    bool m_exit_requested { false };
    u32 m_newest_requested_generation { 0 };

    // Only the decoder thread uses these.
    OwnPtr<VideoDecoder> m_decoder;
    Optional<DecoderError> m_failure;
    u32 m_generation { 0 };
    // The latest submission's, which output the decoder finishes later is taken with as well.
    Optional<AK::Duration> m_output_target;
    Core::AnonymousBuffer m_ring;
};

VideoDecoderServerConnection::VideoDecoderServerConnection(NonnullOwnPtr<IPC::Transport> transport, ReadonlySpan<VideoDecoderRegistration> hosted_decoders)
    : IPC::ConnectionFromClient<VideoDecoderClientEndpoint, VideoDecoderServerEndpoint>(*this, move(transport), 1)
    , m_event_loop(Core::EventLoop::current())
    , m_hosted_decoders(hosted_decoders)
{
}

VideoDecoderServerConnection::~VideoDecoderServerConnection()
{
    revoke_weak_refs();
}

void VideoDecoderServerConnection::die()
{
    revoke_weak_refs();
    // The client may still be showing frames in the slots held for it, so nothing may decode into them anymore.
    for (auto& entry : m_sessions)
        entry.value->exit_and_join();
    m_sessions.clear();

    HashMap<u64, NonnullRefPtr<VideoFrame>> held_frames;
    {
        MutexLocker locker { m_held_frames_mutex };
        held_frames = move(m_held_frames);
    }
    held_frames.clear();

    if (on_death)
        on_death();
}

void VideoDecoderServerConnection::close_after_decoded_frame_count(u32 frame_count)
{
    m_frames_until_close.store(frame_count, AK::MemoryOrder::memory_order_relaxed);
}

Messages::VideoDecoderServer::DecoderCapabilitiesResponse VideoDecoderServerConnection::decoder_capabilities(ParsedCodec codec)
{
    if (track_type_from_codec_id(codec.codec_id()) != TrackType::Video)
        return Optional<DecoderCapabilities> {};
    for (auto const& registration : m_hosted_decoders) {
        if (auto capabilities = registration.capabilities(codec); capabilities.has_value())
            return capabilities;
    }
    return Optional<DecoderCapabilities> {};
}

void VideoDecoderServerConnection::create_decoder(u64 decoder_id, u32 generation, CodecID codec_id, ByteBuffer codec_initialization_data)
{
    if (m_sessions.contains(decoder_id)) {
        did_misbehave("create_decoder: decoder ID already exists");
        return;
    }
    auto session = DecoderSession::create(make_weak_ref(), m_event_loop, m_hosted_decoders, decoder_id, generation, codec_id, move(codec_initialization_data));
    if (session.is_error()) {
        async_decoder_failed(decoder_id, generation, DecoderError::format(DecoderErrorCategory::Memory, "Could not start a video decoder: {}", session.error()));
        return;
    }
    m_sessions.set(decoder_id, session.release_value());
}

void VideoDecoderServerConnection::destroy_decoder(u64 decoder_id)
{
    if (auto session = m_sessions.take(decoder_id); session.has_value())
        (*session)->exit_and_detach();
}

void VideoDecoderServerConnection::replace_coded_data_ring(u64 decoder_id, Core::AnonymousBuffer ring)
{
    if (auto session = m_sessions.get(decoder_id); session.has_value())
        (*session)->enqueue(DecoderSession::ReplaceRing { move(ring) });
}

void VideoDecoderServerConnection::submit_coded_frame(u64 decoder_id, u32 generation, u64 ring_offset, u64 size, AK::Duration presentation_timestamp, AK::Duration decode_timestamp, AK::Duration duration, FrameFlags flags, Optional<ByteBuffer> new_codec_configuration, DecodeIntent intent, Optional<AK::Duration> output_target)
{
    auto session = m_sessions.get(decoder_id);
    if (!session.has_value())
        return;
    (*session)->enqueue(DecoderSession::Submission {
        .generation = generation,
        .ring_offset = ring_offset,
        .size = size,
        .presentation_timestamp = presentation_timestamp,
        .decode_timestamp = decode_timestamp,
        .duration = duration,
        .flags = flags,
        .new_codec_configuration = move(new_codec_configuration),
        .intent = intent,
        .output_target = output_target,
    });
}

void VideoDecoderServerConnection::signal_end_of_stream(u64 decoder_id, u32 generation)
{
    if (auto session = m_sessions.get(decoder_id); session.has_value())
        (*session)->enqueue(DecoderSession::EndOfStream { generation });
}

void VideoDecoderServerConnection::flush(u64 decoder_id, u32 generation)
{
    if (auto session = m_sessions.get(decoder_id); session.has_value())
        (*session)->enqueue(DecoderSession::Flush { generation });
}

void VideoDecoderServerConnection::release_frame(u64 frame_id)
{
    // The frame is dropped outside the lock, since freeing its slot wakes the decoder thread that may be taking it.
    Optional<NonnullRefPtr<VideoFrame>> frame;
    {
        MutexLocker locker { m_held_frames_mutex };
        frame = m_held_frames.take(frame_id);
    }
}

u64 VideoDecoderServerConnection::hold_frame_until_released(NonnullRefPtr<VideoFrame> frame)
{
    MutexLocker locker { m_held_frames_mutex };
    auto frame_id = m_next_held_frame_id++;
    m_held_frames.set(frame_id, move(frame));
    return frame_id;
}

bool VideoDecoderServerConnection::should_close_after_decoding_frame()
{
    auto frames_until_close = m_frames_until_close.load(AK::MemoryOrder::memory_order_relaxed);
    while (true) {
        if (frames_until_close == 0)
            return false;
        if (m_frames_until_close.compare_exchange_strong(frames_until_close, frames_until_close - 1, AK::MemoryOrder::memory_order_relaxed))
            break;
    }
    return frames_until_close == 1;
}

}
