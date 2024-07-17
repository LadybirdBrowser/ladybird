/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/SourceLocation.h>
#include <LibAudio/PlaybackStreamAudioUnit.h>
#include <LibCore/SharedCircularQueue.h>
#include <LibCore/ThreadedPromise.h>

#include <AudioUnit/AudioUnit.h>

namespace Audio {

static constexpr AudioUnitElement AUDIO_UNIT_OUTPUT_BUS = 0;

static void log_os_error_code(OSStatus error_code, SourceLocation location = SourceLocation::current());

#define AU_TRY(expression)                                                         \
    ({                                                                             \
        /* Ignore -Wshadow to allow nesting the macro. */                          \
        AK_IGNORE_DIAGNOSTIC("-Wshadow", auto&& _temporary_result = (expression)); \
        if (_temporary_result != noErr) [[unlikely]] {                             \
            log_os_error_code(_temporary_result);                                  \
            return Error::from_errno(_temporary_result);                           \
        }                                                                          \
    })

struct AudioTask {
    enum class Type {
        Play,
        Pause,
        PauseAndDiscard,
        Volume,
    };

    void resolve(AK::Duration time)
    {
        promise.visit(
            [](Empty) { VERIFY_NOT_REACHED(); },
            [&](NonnullRefPtr<Core::ThreadedPromise<void>>& promise) {
                promise->resolve();
            },
            [&](NonnullRefPtr<Core::ThreadedPromise<AK::Duration>>& promise) {
                promise->resolve(move(time));
            });
    }

    void reject(OSStatus error)
    {
        log_os_error_code(error);

        promise.visit(
            [](Empty) { VERIFY_NOT_REACHED(); },
            [error](auto& promise) {
                promise->reject(Error::from_errno(error));
            });
    }

    Type type;
    Variant<Empty, NonnullRefPtr<Core::ThreadedPromise<void>>, NonnullRefPtr<Core::ThreadedPromise<AK::Duration>>> promise;
    Optional<double> data {};
};

class AudioState : public RefCounted<AudioState> {
public:
    using AudioTaskQueue = Core::SharedSingleProducerCircularQueue<AudioTask>;

    static ErrorOr<NonnullRefPtr<AudioState>> create(AudioStreamBasicDescription description, PlaybackStream::AudioDataRequestCallback data_request_callback, OutputState initial_output_state)
    {
        auto task_queue = TRY(AudioTaskQueue::create());
        auto state = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) AudioState(description, move(task_queue), move(data_request_callback), initial_output_state)));

        AudioComponentDescription component_description;
        component_description.componentType = kAudioUnitType_Output;
        component_description.componentSubType = kAudioUnitSubType_DefaultOutput;
        component_description.componentManufacturer = kAudioUnitManufacturer_Apple;
        component_description.componentFlags = 0;
        component_description.componentFlagsMask = 0;

        auto* component = AudioComponentFindNext(NULL, &component_description);
        AU_TRY(AudioComponentInstanceNew(component, &state->m_audio_unit));

        AU_TRY(AudioUnitSetProperty(
            state->m_audio_unit,
            kAudioUnitProperty_StreamFormat,
            kAudioUnitScope_Input,
            AUDIO_UNIT_OUTPUT_BUS,
            &description,
            sizeof(description)));

        AURenderCallbackStruct callbackStruct;
        callbackStruct.inputProc = &AudioState::on_audio_unit_buffer_request;
        callbackStruct.inputProcRefCon = state.ptr();

        AU_TRY(AudioUnitSetProperty(
            state->m_audio_unit,
            kAudioUnitProperty_SetRenderCallback,
            kAudioUnitScope_Global,
            AUDIO_UNIT_OUTPUT_BUS,
            &callbackStruct,
            sizeof(callbackStruct)));

        AU_TRY(AudioUnitInitialize(state->m_audio_unit));
        AU_TRY(AudioOutputUnitStart(state->m_audio_unit));

        return state;
    }

    ~AudioState()
    {
        if (m_audio_unit != nullptr)
            AudioOutputUnitStop(m_audio_unit);
    }

    ErrorOr<void> queue_task(AudioTask task)
    {
        return m_task_queue.blocking_enqueue(move(task), []() {
            usleep(10'000);
        });
    }

    AK::Duration last_sample_time() const
    {
        return AK::Duration::from_milliseconds(m_last_sample_time.load());
    }

private:
    AudioState(AudioStreamBasicDescription description, AudioTaskQueue task_queue, PlaybackStream::AudioDataRequestCallback data_request_callback, OutputState initial_output_state)
        : m_description(description)
        , m_task_queue(move(task_queue))
        , m_paused(initial_output_state == OutputState::Playing ? Paused::No : Paused::Yes)
        , m_data_request_callback(move(data_request_callback))
    {
    }

    static OSStatus on_audio_unit_buffer_request(void* user_data, AudioUnitRenderActionFlags*, AudioTimeStamp const* time_stamp, UInt32 element, UInt32 frames_to_render, AudioBufferList* output_buffer_list)
    {
        VERIFY(element == AUDIO_UNIT_OUTPUT_BUS);
        VERIFY(output_buffer_list->mNumberBuffers == 1);

        auto& state = *static_cast<AudioState*>(user_data);

        VERIFY(time_stamp->mFlags & kAudioTimeStampSampleTimeValid);
        auto sample_time_seconds = time_stamp->mSampleTime / state.m_description.mSampleRate;

        auto last_sample_time = static_cast<i64>(sample_time_seconds * 1000.0);
        state.m_last_sample_time.store(last_sample_time);

        if (auto result = state.m_task_queue.dequeue(); result.is_error()) {
            VERIFY(result.error() == AudioTaskQueue::QueueStatus::Empty);
        } else {
            auto task = result.release_value();
            OSStatus error = noErr;

            switch (task.type) {
            case AudioTask::Type::Play:
                state.m_paused = Paused::No;
                break;

            case AudioTask::Type::Pause:
                state.m_paused = Paused::Yes;
                break;

            case AudioTask::Type::PauseAndDiscard:
                error = AudioUnitReset(state.m_audio_unit, kAudioUnitScope_Global, AUDIO_UNIT_OUTPUT_BUS);
                state.m_paused = Paused::Yes;
                break;

            case AudioTask::Type::Volume:
                VERIFY(task.data.has_value());
                error = AudioUnitSetParameter(state.m_audio_unit, kHALOutputParam_Volume, kAudioUnitScope_Global, 0, static_cast<float>(*task.data), 0);
                break;
            }

            if (error == noErr)
                task.resolve(AK::Duration::from_milliseconds(last_sample_time));
            else
                task.reject(error);
        }

        Bytes output_buffer {
            reinterpret_cast<u8*>(output_buffer_list->mBuffers[0].mData),
            output_buffer_list->mBuffers[0].mDataByteSize
        };

        if (state.m_paused == Paused::No) {
            auto written_bytes = state.m_data_request_callback(output_buffer, PcmSampleFormat::Float32, frames_to_render);

            if (written_bytes.is_empty())
                state.m_paused = Paused::Yes;
        }

        if (state.m_paused == Paused::Yes)
            output_buffer.fill(0);

        return noErr;
    }

    AudioComponentInstance m_audio_unit { nullptr };
    AudioStreamBasicDescription m_description {};

    AudioTaskQueue m_task_queue;

    enum class Paused {
        Yes,
        No,
    };
    Paused m_paused { Paused::Yes };

    PlaybackStream::AudioDataRequestCallback m_data_request_callback;
    Atomic<i64> m_last_sample_time { 0 };
};

ErrorOr<NonnullRefPtr<PlaybackStream>> PlaybackStreamAudioUnit::create(OutputState initial_output_state, u32 sample_rate, u8 channels, u32, AudioDataRequestCallback&& data_request_callback)
{
    AudioStreamBasicDescription description {};
    description.mFormatID = kAudioFormatLinearPCM;
    description.mFormatFlags = kLinearPCMFormatFlagIsFloat | kLinearPCMFormatFlagIsPacked;
    description.mSampleRate = sample_rate;
    description.mChannelsPerFrame = channels;
    description.mBitsPerChannel = sizeof(float) * 8;
    description.mBytesPerFrame = sizeof(float) * channels;
    description.mBytesPerPacket = description.mBytesPerFrame;
    description.mFramesPerPacket = 1;

    auto state = TRY(AudioState::create(description, move(data_request_callback), initial_output_state));
    return TRY(adopt_nonnull_ref_or_enomem(new (nothrow) PlaybackStreamAudioUnit(move(state))));
}

PlaybackStreamAudioUnit::PlaybackStreamAudioUnit(NonnullRefPtr<AudioState> impl)
    : m_state(move(impl))
{
}

PlaybackStreamAudioUnit::~PlaybackStreamAudioUnit() = default;

void PlaybackStreamAudioUnit::set_underrun_callback(Function<void()>)
{
    // FIXME: Implement this.
}

NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> PlaybackStreamAudioUnit::resume()
{
    auto promise = Core::ThreadedPromise<AK::Duration>::create();
    AudioTask task { AudioTask::Type::Play, promise };

    if (auto result = m_state->queue_task(move(task)); result.is_error())
        promise->reject(result.release_error());

    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamAudioUnit::drain_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    AudioTask task { AudioTask::Type::Pause, promise };

    if (auto result = m_state->queue_task(move(task)); result.is_error())
        promise->reject(result.release_error());

    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamAudioUnit::discard_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    AudioTask task { AudioTask::Type::PauseAndDiscard, promise };

    if (auto result = m_state->queue_task(move(task)); result.is_error())
        promise->reject(result.release_error());

    return promise;
}

ErrorOr<AK::Duration> PlaybackStreamAudioUnit::total_time_played()
{
    return m_state->last_sample_time();
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamAudioUnit::set_volume(double volume)
{
    auto promise = Core::ThreadedPromise<void>::create();
    AudioTask task { AudioTask::Type::Volume, promise, volume };

    if (auto result = m_state->queue_task(move(task)); result.is_error())
        promise->reject(result.release_error());

    return promise;
}

void log_os_error_code([[maybe_unused]] OSStatus error_code, [[maybe_unused]] SourceLocation location)
{
#if AUDIO_DEBUG
    auto error_string = "Unknown error"sv;

    // Errors listed in AUComponent.h
    switch (error_code) {
    case kAudioUnitErr_InvalidProperty:
        error_string = "InvalidProperty"sv;
        break;
    case kAudioUnitErr_InvalidParameter:
        error_string = "InvalidParameter"sv;
        break;
    case kAudioUnitErr_InvalidElement:
        error_string = "InvalidElement"sv;
        break;
    case kAudioUnitErr_NoConnection:
        error_string = "NoConnection"sv;
        break;
    case kAudioUnitErr_FailedInitialization:
        error_string = "FailedInitialization"sv;
        break;
    case kAudioUnitErr_TooManyFramesToProcess:
        error_string = "TooManyFramesToProcess"sv;
        break;
    case kAudioUnitErr_InvalidFile:
        error_string = "InvalidFile"sv;
        break;
    case kAudioUnitErr_UnknownFileType:
        error_string = "UnknownFileType"sv;
        break;
    case kAudioUnitErr_FileNotSpecified:
        error_string = "FileNotSpecified"sv;
        break;
    case kAudioUnitErr_FormatNotSupported:
        error_string = "FormatNotSupported"sv;
        break;
    case kAudioUnitErr_Uninitialized:
        error_string = "Uninitialized"sv;
        break;
    case kAudioUnitErr_InvalidScope:
        error_string = "InvalidScope"sv;
        break;
    case kAudioUnitErr_PropertyNotWritable:
        error_string = "PropertyNotWritable"sv;
        break;
    case kAudioUnitErr_CannotDoInCurrentContext:
        error_string = "CannotDoInCurrentContext"sv;
        break;
    case kAudioUnitErr_InvalidPropertyValue:
        error_string = "InvalidPropertyValue"sv;
        break;
    case kAudioUnitErr_PropertyNotInUse:
        error_string = "PropertyNotInUse"sv;
        break;
    case kAudioUnitErr_Initialized:
        error_string = "Initialized"sv;
        break;
    case kAudioUnitErr_InvalidOfflineRender:
        error_string = "InvalidOfflineRender"sv;
        break;
    case kAudioUnitErr_Unauthorized:
        error_string = "Unauthorized"sv;
        break;
    case kAudioUnitErr_MIDIOutputBufferFull:
        error_string = "MIDIOutputBufferFull"sv;
        break;
    case kAudioComponentErr_InstanceTimedOut:
        error_string = "InstanceTimedOut"sv;
        break;
    case kAudioComponentErr_InstanceInvalidated:
        error_string = "InstanceInvalidated"sv;
        break;
    case kAudioUnitErr_RenderTimeout:
        error_string = "RenderTimeout"sv;
        break;
    case kAudioUnitErr_ExtensionNotFound:
        error_string = "ExtensionNotFound"sv;
        break;
    case kAudioUnitErr_InvalidParameterValue:
        error_string = "InvalidParameterValue"sv;
        break;
    case kAudioUnitErr_InvalidFilePath:
        error_string = "InvalidFilePath"sv;
        break;
    case kAudioUnitErr_MissingKey:
        error_string = "MissingKey"sv;
        break;
    default:
        break;
    }

    warnln("{}: Audio Unit error {}: {}", location, error_code, error_string);
#endif
}

}
