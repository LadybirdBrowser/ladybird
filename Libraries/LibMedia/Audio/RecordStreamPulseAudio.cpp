/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/Audio/PulseAudioWrappers.h>
#include <LibMedia/Audio/RecordStream.h>
#include <LibThreading/Thread.h>

namespace Audio {

namespace {

class RecordStreamPulseAudio final : public RecordStream {
public:
    explicit RecordStreamPulseAudio(NonnullRefPtr<PulseAudioRecordStream> stream)
        : m_stream(move(stream))
    {
    }

    virtual SampleSpecification const& sample_specification() const override { return m_stream->sample_specification(); }

private:
    NonnullRefPtr<PulseAudioRecordStream> m_stream;
};

}

static ErrorOr<NonnullRefPtr<RecordStream>> create_pulse_audio_record_stream(SampleSpecification const& specification, u32 fragment_size_bytes, ByteString device_name, RecordStream::RecordCallback callback)
{
    auto context = TRY(PulseAudioContext::the());

    auto pulse_stream = TRY(context->create_record_stream(specification, fragment_size_bytes,
        device_name.is_empty() ? nullptr : device_name.characters(), move(callback)));

    auto stream = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) RecordStreamPulseAudio(move(pulse_stream))));
    return stream;
}

NonnullRefPtr<RecordStream::CreatePromise> RecordStream::create(SampleSpecification const& specification, u32 fragment_size_bytes, StringView device_id, RecordCallback callback)
{
    auto promise = CreatePromise::construct();

    // AudioDevices encodes PulseAudio sources as "pulse:source:<name>". Unrecognized or
    // empty ids fall through to the server's default source.
    constexpr auto pulse_source_prefix = "pulse:source:"sv;
    ByteString device_name;
    if (device_id.starts_with(pulse_source_prefix))
        device_name = device_id.substring_view(pulse_source_prefix.length());

    if (!Core::EventLoop::is_running()) {
        auto stream_or_error = create_pulse_audio_record_stream(specification, fragment_size_bytes, move(device_name), move(callback));
        if (stream_or_error.is_error())
            promise->reject(stream_or_error.release_error());
        else
            promise->resolve(stream_or_error.release_value());
        return promise;
    }

    auto& main_thread_event_loop = Core::EventLoop::current();
    auto thread_or_error = Threading::Thread::try_create("AudioCaptureCtl"sv, [&main_thread_event_loop, promise, specification, fragment_size_bytes, device_name = move(device_name), callback = move(callback)]() mutable {
        auto stream_or_error = create_pulse_audio_record_stream(specification, fragment_size_bytes, move(device_name), move(callback));
        main_thread_event_loop.deferred_invoke([promise, stream_or_error = move(stream_or_error)]() mutable {
            if (stream_or_error.is_error())
                promise->reject(stream_or_error.release_error());
            else
                promise->resolve(stream_or_error.release_value());
        });
        return 0;
    });
    if (thread_or_error.is_error()) {
        promise->reject(thread_or_error.release_error());
        return promise;
    }
    auto thread = thread_or_error.release_value();
    thread->start();
    thread->detach();
    return promise;
}

}
