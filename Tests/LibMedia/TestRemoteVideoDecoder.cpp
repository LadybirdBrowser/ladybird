/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/ConditionVariable.h>
#include <AK/Mutex.h>
#include <AK/ScopeGuard.h>
#include <AK/StringHash.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibMedia/DecoderRegistrations.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/Producers/DecodedVideoProducer.h>
#include <LibMedia/RemoteVideoDecoder.h>
#include <LibMediaCodecs/FFmpeg/FFmpegVideoDecoder.h>
#include <LibMediaCodecs/VideoDecoderServerConnection.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibThreading/Thread.h>
#include <Tests/LibMedia/TestMediaCommon.h>
#include <Tests/LibMedia/WithholdingVideoDecoder.h>

struct FrameDigest {
    AK::Duration timestamp;
    u32 pixel_hash { 0 };

    bool operator==(FrameDigest const&) const = default;
};

template<>
struct AK::Formatter<FrameDigest> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, FrameDigest const& digest)
    {
        return Formatter<FormatString>::format(builder, "{}ms:{:08x}"sv, digest.timestamp.to_milliseconds(), digest.pixel_hash);
    }
};

namespace {

Media::DecoderErrorOr<NonnullOwnPtr<Media::VideoDecoder>> create_software_video_decoder(Media::CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    return NonnullOwnPtr<Media::VideoDecoder> { TRY(Media::FFmpeg::FFmpegVideoDecoder::try_create(codec_id, codec_initialization_data)) };
}

ReadonlySpan<Media::VideoDecoderRegistration> hosted_decoders()
{
    // The withholding decoder answers only while a test is withholding outputs.
    static Array decoders {
        WithholdingVideoDecoder::registrations()[0],
        Media::VideoDecoderRegistration { Media::FFmpeg::FFmpegVideoDecoder::capabilities, create_software_video_decoder },
    };
    return decoders;
}

Atomic<u32> s_server_launch_count { 0 };

// The server that is running, if any. Its connection is only touched on its own thread.
Mutex s_running_server_mutex;
Core::EventLoop* s_running_server_event_loop { nullptr };
Media::VideoDecoderServerConnection* s_running_server_connection { nullptr };
u32 s_close_every_server_after_frames { 0 };

// Hosts a server on a thread of its own, as the VideoDecoderServer process would.
ErrorOr<NonnullOwnPtr<IPC::Transport>> launch_in_process_server()
{
    auto paired_transports = TRY(IPC::Transport::create_paired());
    s_server_launch_count++;

    auto thread = TRY(Threading::Thread::try_create("Video Decoder Server"sv, [transport = move(paired_transports.local)]() mutable -> intptr_t {
        Core::EventLoop loop;
        auto connection = Media::VideoDecoderServerConnection::construct(move(transport), hosted_decoders());
        {
            MutexLocker locker { s_running_server_mutex };
            if (s_close_every_server_after_frames > 0)
                connection->close_after_decoded_frame_count(s_close_every_server_after_frames);
            s_running_server_event_loop = &loop;
            s_running_server_connection = connection.ptr();
        }
        connection->on_death = [&loop] {
            MutexLocker locker { s_running_server_mutex };
            if (s_running_server_event_loop == &loop) {
                s_running_server_event_loop = nullptr;
                s_running_server_connection = nullptr;
            }
            loop.quit(0);
        };
        return loop.exec();
    }));
    thread->start();
    thread->detach();
    return paired_transports.remote_handle.create_transport();
}

// Applies to the running server, and to every one launched after it while the count is set.
void close_servers_after_decoded_frames(u32 frame_count)
{
    MutexLocker locker { s_running_server_mutex };
    s_close_every_server_after_frames = frame_count;
    if (s_running_server_event_loop == nullptr)
        return;
    s_running_server_event_loop->deferred_invoke([frame_count] {
        MutexLocker locker { s_running_server_mutex };
        if (s_running_server_connection != nullptr)
            s_running_server_connection->close_after_decoded_frame_count(frame_count);
    });
}

void use_remote_decoders()
{
    Media::install_video_decoders({ Media::remote_video_decoders(), Media::ffmpeg_video_decoders() });
}

void use_local_decoders()
{
    Media::install_video_decoders({ Media::ffmpeg_video_decoders() });
}

FrameDigest digest_frame(Media::VideoFrame const& frame)
{
    auto yuv_data = frame.yuv_data();
    VERIFY(yuv_data.has_value());
    auto hash = string_hash(reinterpret_cast<char const*>(yuv_data->y_data().data()), yuv_data->y_data().size());
    hash = string_hash(reinterpret_cast<char const*>(yuv_data->u_data().data()), yuv_data->u_data().size(), hash);
    hash = string_hash(reinterpret_cast<char const*>(yuv_data->v_data().data()), yuv_data->v_data().size(), hash);
    return { frame.timestamp(), hash };
}

NonnullRefPtr<Media::DecodedVideoProducer> create_started_producer(Core::EventLoop& loop, StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    NonnullRefPtr<Media::Demuxer> demuxer = MUST(Media::Matroska::MatroskaDemuxer::from_stream(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video)).release_value();
    auto producer = MUST(Media::DecodedVideoProducer::try_create(loop, demuxer, track));
    producer->set_error_handler([](Media::DecoderError&& error) {
        FAIL(ByteString::formatted("An error occurred while decoding: {}", error.description()));
    });
    producer->start();
    return producer;
}

Optional<FrameDigest> take_frame(Media::DecodedVideoProducer& producer, Core::EventLoop& loop)
{
    auto deadline = MonotonicTime::now_coarse() + AK::Duration::from_seconds(5);
    while (MonotonicTime::now_coarse() < deadline) {
        auto output = producer.peek();
        if (output.status == Media::PipelineStatus::HaveData) {
            auto digest = digest_frame(*output.frame);
            producer.consume();
            return digest;
        }
        if (output.status == Media::PipelineStatus::EndOfStream || output.status == Media::PipelineStatus::Error)
            return {};
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }
    FAIL("Timed out waiting for a frame");
    return {};
}

// Frames submitted again after a decoder loses its state can repeat ones that were already delivered, which the
// sinks ignore, since they show nothing new.
void append_unless_repeated(Vector<FrameDigest>& digests, FrameDigest const& digest)
{
    if (!digests.is_empty() && digest.timestamp <= digests.last().timestamp)
        return;
    digests.append(digest);
}

Vector<FrameDigest> decode_every_frame(StringView path)
{
    auto& loop = never_destroyed_event_loop();
    auto producer = create_started_producer(loop, path);
    Vector<FrameDigest> digests;
    while (true) {
        auto digest = take_frame(*producer, loop);
        if (!digest.has_value())
            break;
        append_unless_repeated(digests, *digest);
    }
    return digests;
}

Vector<FrameDigest> decode_every_frame_locally(StringView path)
{
    use_local_decoders();
    auto digests = decode_every_frame(path);
    use_remote_decoders();
    return digests;
}

// The Browser is asked for transports from the MediaServer's main thread, which never asks for one itself. The tests
// ask from this thread, so the requester gets a thread of its own, as the Browser's answers would arrive on.
void install_transport_requester_on_its_own_thread()
{
    IGNORE_USE_IN_ESCAPING_LAMBDA Mutex mutex;
    IGNORE_USE_IN_ESCAPING_LAMBDA ConditionVariable condition { mutex };
    IGNORE_USE_IN_ESCAPING_LAMBDA bool requester_is_installed = false;
    auto thread = MUST(Threading::Thread::try_create("Transport Requester"sv, [&]() -> intptr_t {
        Core::EventLoop loop;
        Media::RemoteVideoDecoder::set_transport_requester([](auto callback) { callback(launch_in_process_server()); });
        {
            MutexLocker locker { mutex };
            requester_is_installed = true;
            condition.broadcast();
        }
        return loop.exec();
    }));
    thread->start();
    thread->detach();

    MutexLocker locker { mutex };
    while (!requester_is_installed)
        condition.wait();
}

constexpr auto test_video_path = "./vp9_in_webm.webm"sv;

}

TEST_SETUP
{
    (void)never_destroyed_event_loop();
    install_transport_requester_on_its_own_thread();
    use_remote_decoders();
    Media::install_audio_decoders({ Media::audio_decoders() });
}

TEST_CASE(frames_decoded_by_the_server_match_local_decoding)
{
    auto expected = decode_every_frame_locally(test_video_path);
    EXPECT(expected.size() > 10);

    auto launches_before = s_server_launch_count.load();
    auto actual = decode_every_frame(test_video_path);
    EXPECT_EQ(actual, expected);
    EXPECT(s_server_launch_count.load() - launches_before <= 1);
}

TEST_CASE(a_lost_server_is_relaunched_and_decoding_resumes_where_it_left_off)
{
    auto expected = decode_every_frame_locally(test_video_path);

    auto launches_before = s_server_launch_count.load();
    close_servers_after_decoded_frames(7);
    auto& loop = never_destroyed_event_loop();
    auto producer = create_started_producer(loop, test_video_path);
    Vector<FrameDigest> actual;
    while (true) {
        auto digest = take_frame(*producer, loop);
        if (!digest.has_value())
            break;
        append_unless_repeated(actual, *digest);
        // Only the first server is lost.
        if (s_server_launch_count.load() > launches_before)
            close_servers_after_decoded_frames(0);
    }
    EXPECT_EQ(actual, expected);
    EXPECT(s_server_launch_count.load() > launches_before);
}

TEST_CASE(a_decoder_that_keeps_losing_its_server_falls_back_to_the_next_decoder)
{
    auto expected = decode_every_frame_locally(test_video_path);

    auto launches_before = s_server_launch_count.load();
    bool server_was_running = false;
    {
        MutexLocker locker { s_running_server_mutex };
        server_was_running = s_running_server_event_loop != nullptr;
    }
    close_servers_after_decoded_frames(3);
    auto actual = decode_every_frame(test_video_path);
    close_servers_after_decoded_frames(0);
    EXPECT_EQ(actual, expected);

    // Every server it lost was replaced, except the last.
    auto expected_launches = Media::RemoteVideoDecoder::MAXIMUM_SERVER_LOSSES;
    if (!server_was_running)
        expected_launches++;
    EXPECT_EQ(s_server_launch_count.load() - launches_before, expected_launches);
}

TEST_CASE(a_seek_discards_the_frames_in_flight)
{
    auto expected = decode_every_frame_locally(test_video_path);
    VERIFY(expected.size() > 20);

    auto& loop = never_destroyed_event_loop();
    auto producer = create_started_producer(loop, test_video_path);
    for (size_t index = 0; index < 3; index++)
        EXPECT_EQ(take_frame(*producer, loop), expected[index]);

    // Seeking backward leaves the frames already submitted for later timestamps in flight.
    auto const& target = expected[1];
    producer->seek(target.timestamp, Media::InvalidateHeldData::Yes);
    EXPECT_EQ(take_frame(*producer, loop), target);
    EXPECT_EQ(take_frame(*producer, loop), expected[2]);
}

namespace {

// A decoder whose server withholds as many outputs as it can, and so has stopped answering: the last submissions in
// flight stay unanswered until the test releases an output.
struct StalledDecoding {
    NonnullRefPtr<WithheldOutputs> withheld_outputs;
    NonnullOwnPtr<Media::RemoteVideoDecoder> decoder;
};

StalledDecoding submit_until_the_server_stops_answering(size_t unanswered_submission_count)
{
    auto withheld_outputs = make_ref_counted<WithheldOutputs>();
    WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(withheld_outputs);
    ScopeGuard stop_withholding = [] { WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(nullptr); };

    auto file = MUST(Core::File::open(test_video_path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    NonnullRefPtr<Media::Demuxer> demuxer = MUST(Media::Matroska::MatroskaDemuxer::from_stream(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video)).release_value();
    MUST(demuxer->create_context_for_track(track));

    auto first_coded_frame = MUST(demuxer->get_next_sample_for_track(track));
    auto decoder = MUST(Media::RemoteVideoDecoder::try_create(first_coded_frame.codec_id(), first_coded_frame.new_codec_configuration().value()));
    decoder->set_output_arrived_callback([] { });

    MUST(decoder->receive_coded_data(first_coded_frame, Media::DecodeIntent::Output));
    for (size_t index = 1; index < WithholdingVideoDecoder::MAXIMUM_WITHHELD_OUTPUTS + unanswered_submission_count; index++)
        MUST(decoder->receive_coded_data(MUST(demuxer->get_next_sample_for_track(track)), Media::DecodeIntent::Output));
    EXPECT(withheld_outputs->wait_until_withheld_count_reaches(WithholdingVideoDecoder::MAXIMUM_WITHHELD_OUTPUTS, AK::Duration::from_seconds(5)));
    return { move(withheld_outputs), move(decoder) };
}

// Takes output on a thread of its own, so that a wait inside the call can be observed and ended from the test.
struct OutputTaker {
    explicit OutputTaker(Media::RemoteVideoDecoder& decoder)
        : thread(MUST(Threading::Thread::try_create("Taker"sv, [this, &decoder]() -> intptr_t {
            auto result = decoder.take_next_output({});
            MutexLocker locker { mutex };
            answer = result.is_error() ? result.error().category() : Media::DecoderErrorCategory::Unknown;
            condition.broadcast();
            return 0;
        })))
    {
        thread->start();
    }

    ~OutputTaker() { (void)thread->join(); }

    Optional<Media::DecoderErrorCategory> wait_for_answer(AK::Duration timeout)
    {
        auto deadline = MonotonicTime::now() + timeout;
        MutexLocker locker { mutex };
        while (!answer.has_value() && MonotonicTime::now() < deadline)
            condition.wait_for(deadline - MonotonicTime::now());
        return answer;
    }

    Mutex mutex;
    ConditionVariable condition { mutex };
    Optional<Media::DecoderErrorCategory> answer;
    NonnullRefPtr<Threading::Thread> thread;
};

}

// The server answers the submissions a flush supersedes only once its decoder lets it, so the frames submitted after
// the flush must not wait behind them.
TEST_CASE(superseded_submissions_are_not_waited_for_after_a_flush)
{
    auto stalled = submit_until_the_server_stops_answering(Media::RemoteVideoDecoder::MAXIMUM_FRAMES_IN_FLIGHT);
    stalled.decoder->flush();

    {
        OutputTaker taker { *stalled.decoder };
        auto answer = taker.wait_for_answer(AK::Duration::from_seconds(2));
        if (!answer.has_value())
            FAIL("The frames submitted before the flush were waited for");
        else
            EXPECT_EQ(*answer, Media::DecoderErrorCategory::NeedsMoreInput);
        // Letting the server's decoder finish answers the superseded submissions and ends the taker either way.
        while (stalled.withheld_outputs->release_next()) { }
    }
}

// A frame the server's decoder finishes after the server answered its submission has to reach the client, and wake
// it, without the client submitting anything else.
TEST_CASE(output_the_server_decoder_finishes_later_wakes_the_client)
{
    auto withheld_outputs = make_ref_counted<WithheldOutputs>();
    WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(withheld_outputs);
    ScopeGuard stop_withholding = [] { WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(nullptr); };

    auto file = MUST(Core::File::open(test_video_path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    NonnullRefPtr<Media::Demuxer> demuxer = MUST(Media::Matroska::MatroskaDemuxer::from_stream(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video)).release_value();
    TRY_OR_FAIL(demuxer->create_context_for_track(track));

    Mutex wake_mutex;
    ConditionVariable wake_condition { wake_mutex };
    size_t wake_count = 0;

    auto first_coded_frame = TRY_OR_FAIL(demuxer->get_next_sample_for_track(track));
    auto decoder = TRY_OR_FAIL(Media::RemoteVideoDecoder::try_create(first_coded_frame.codec_id(), first_coded_frame.new_codec_configuration().value()));
    decoder->set_output_arrived_callback([&] {
        MutexLocker locker { wake_mutex };
        wake_count++;
        wake_condition.broadcast();
    });

    constexpr size_t submitted_frame_count = 3;
    TRY_OR_FAIL(decoder->receive_coded_data(first_coded_frame, Media::DecodeIntent::Output));
    for (size_t index = 1; index < submitted_frame_count; index++)
        TRY_OR_FAIL(decoder->receive_coded_data(TRY_OR_FAIL(demuxer->get_next_sample_for_track(track)), Media::DecodeIntent::Output));

    auto result = decoder->take_next_output({});
    EXPECT(result.is_error() && result.error().category() == Media::DecoderErrorCategory::NeedsMoreInput);
    EXPECT(withheld_outputs->wait_until_withheld_count_reaches(submitted_frame_count, AK::Duration::from_seconds(5)));
    EXPECT_EQ(wake_count, 0u);

    EXPECT(withheld_outputs->release_next());
    {
        auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(5);
        MutexLocker locker { wake_mutex };
        while (wake_count == 0 && MonotonicTime::now() < deadline)
            wake_condition.wait_for(deadline - MonotonicTime::now());
        if (wake_count == 0) {
            FAIL("The client was not woken for the frame the server's decoder finished");
            return;
        }
    }

    auto frame = decoder->take_next_output({});
    if (frame.is_error())
        FAIL(ByteString::formatted("The frame the server's decoder finished was not delivered: {}", frame.error().description()));
    else
        EXPECT_EQ(frame.value()->timestamp(), first_coded_frame.presentation_timestamp());
}

// Once the client holds every slot the server can decode into, the server waits for one to be released, and the
// client answers TryAgain until a release of its own lets the server decode again.
TEST_CASE(a_server_out_of_slots_is_waited_for_until_a_frame_is_released)
{
    auto file = MUST(Core::File::open(test_video_path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    NonnullRefPtr<Media::Demuxer> demuxer = MUST(Media::Matroska::MatroskaDemuxer::from_stream(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video)).release_value();
    TRY_OR_FAIL(demuxer->create_context_for_track(track));

    auto coded_frame = TRY_OR_FAIL(demuxer->get_next_sample_for_track(track));
    auto decoder = TRY_OR_FAIL(Media::RemoteVideoDecoder::try_create(coded_frame.codec_id(), coded_frame.new_codec_configuration().value()));
    size_t storage_freed_count = 0;
    decoder->set_storage_freed_callback([&] { storage_freed_count++; });
    decoder->set_output_arrived_callback([] { });

    Vector<NonnullRefPtr<Media::VideoFrame>> held_frames;
    auto server_ran_out_of_slots = false;
    for (size_t submitted_count = 0; submitted_count < 32 && !server_ran_out_of_slots; submitted_count++) {
        TRY_OR_FAIL(decoder->receive_coded_data(coded_frame, Media::DecodeIntent::Output));
        while (true) {
            auto result = decoder->take_next_output({});
            if (!result.is_error()) {
                held_frames.append(result.release_value());
                continue;
            }
            if (result.error().category() == Media::DecoderErrorCategory::TryAgain) {
                server_ran_out_of_slots = true;
                break;
            }
            if (result.error().category() == Media::DecoderErrorCategory::NeedsMoreInput)
                break;
            FAIL(ByteString::formatted("Unexpected decoder error: {}", result.error().description()));
            return;
        }
        coded_frame = TRY_OR_FAIL(demuxer->get_next_sample_for_track(track));
    }
    EXPECT(server_ran_out_of_slots);
    EXPECT_EQ(held_frames.size(), static_cast<size_t>(Media::VideoFramePool::MAX_SLOT_COUNT));
    EXPECT_EQ(storage_freed_count, 0u);

    // Nothing has changed for the server, so the answer is the same without waiting.
    auto result = decoder->take_next_output({});
    EXPECT(result.is_error() && result.error().category() == Media::DecoderErrorCategory::TryAgain);

    auto last_held_timestamp = held_frames.last()->timestamp();
    held_frames.remove(0);
    EXPECT_EQ(storage_freed_count, 1u);

    auto frame = decoder->take_next_output({});
    if (frame.is_error())
        FAIL(ByteString::formatted("The frame decoded into the released slot was not delivered: {}", frame.error().description()));
    else
        EXPECT(frame.value()->timestamp() > last_held_timestamp);
    EXPECT_EQ(storage_freed_count, 1u);
}
