/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ScopeGuard.h>
#include <AK/SourceLocation.h>
#include <AK/Vector.h>
#include <LibCore/ThreadedPromise.h>
#include <LibMedia/Audio/PlaybackStreamAudioUnit.h>
#include <LibThreading/Mutex.h>

#include <AudioToolbox/AudioFormat.h>
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

static ErrorOr<ChannelMap> audio_channel_layout_to_channel_map(AudioChannelLayout const& channel_layout);

template<typename T>
class CoreAudioPropertyValue {
    AK_MAKE_NONCOPYABLE(CoreAudioPropertyValue);

public:
    static ErrorOr<CoreAudioPropertyValue<T>> create(u32 size)
    {
        auto ptr = reinterpret_cast<T*>(malloc(size));
        if (ptr == nullptr)
            return Error::from_errno(ENOMEM);
        return CoreAudioPropertyValue<T>(ptr, size);
    }

    CoreAudioPropertyValue(T* ptr, u32 size)
        : m_ptr(ptr)
        , m_size(size)
    {
    }
    CoreAudioPropertyValue(CoreAudioPropertyValue&& other)
        : m_ptr(exchange(other.m_ptr, nullptr))
        , m_size(exchange(other.m_size, 0))
    {
    }
    ~CoreAudioPropertyValue()
    {
        free(m_ptr);
    }

    u32 size() const { return m_size; }
    T* ptr() const { return m_ptr; }
    T* operator->() const { return m_ptr; }
    T& operator*() const { return *m_ptr; }

private:
    T* m_ptr;
    u32 m_size;
};

template<typename T>
static ErrorOr<CoreAudioPropertyValue<T>> get_audio_unit_property(AudioComponentInstance& instance, u32 property)
{
    u32 size = 0;
    AU_TRY(AudioUnitGetPropertyInfo(
        instance,
        property,
        kAudioUnitScope_Output,
        AUDIO_UNIT_OUTPUT_BUS,
        &size,
        nullptr));
    VERIFY(size >= sizeof(T));

    auto result = TRY(CoreAudioPropertyValue<T>::create(size));
    AU_TRY(AudioUnitGetProperty(
        instance,
        property,
        kAudioUnitScope_Output,
        AUDIO_UNIT_OUTPUT_BUS,
        result.ptr(),
        &size));
    VERIFY(result.size() == size);
    return result;
}

template<typename T>
static ErrorOr<void> set_audio_unit_property(AudioComponentInstance& instance, u32 property, CoreAudioPropertyValue<T> const& value)
{
    AU_TRY(AudioUnitSetProperty(
        instance,
        property,
        kAudioUnitScope_Input,
        AUDIO_UNIT_OUTPUT_BUS,
        value.ptr(),
        value.size()));
    return {};
}

static void check_audio_channel_layout_size(AudioChannelLayout& layout, u32 size)
{
    auto minimum_layout_size = Checked(layout.mNumberChannelDescriptions);
    minimum_layout_size--;
    minimum_layout_size *= sizeof(layout.mChannelDescriptions[0]);
    minimum_layout_size += sizeof(AudioChannelLayout);
    VERIFY(size >= minimum_layout_size.value());
}

class AudioState : public RefCounted<AudioState> {
public:
    static ErrorOr<NonnullRefPtr<AudioState>> create(PlaybackStream::SampleSpecificationCallback sample_specification_callback, PlaybackStream::AudioDataRequestCallback data_request_callback, OutputState initial_output_state)
    {
        auto state = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) AudioState(move(data_request_callback), initial_output_state)));

        AudioComponentDescription component_description;
        component_description.componentType = kAudioUnitType_Output;
        component_description.componentSubType = kAudioUnitSubType_DefaultOutput;
        component_description.componentManufacturer = kAudioUnitManufacturer_Apple;
        component_description.componentFlags = 0;
        component_description.componentFlagsMask = 0;

        auto* component = AudioComponentFindNext(NULL, &component_description);
        AU_TRY(AudioComponentInstanceNew(component, &state->m_audio_unit));

        auto description = TRY(get_audio_unit_property<AudioStreamBasicDescription>(state->m_audio_unit, kAudioUnitProperty_StreamFormat));

        description->mFormatID = kAudioFormatLinearPCM;
        description->mFormatFlags = kLinearPCMFormatFlagIsFloat | kLinearPCMFormatFlagIsPacked;

        TRY(set_audio_unit_property(state->m_audio_unit, kAudioUnitProperty_StreamFormat, description));

        auto layout = TRY(get_audio_unit_property<AudioChannelLayout>(state->m_audio_unit, kAudioUnitProperty_AudioChannelLayout));
        check_audio_channel_layout_size(*layout, layout.size());
        auto channel_map = TRY(audio_channel_layout_to_channel_map(*layout));
        state->m_sample_specification = SampleSpecification(static_cast<u32>(description->mSampleRate), channel_map);

        sample_specification_callback(state->m_sample_specification);

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

    void queue_task(AudioTask task)
    {
        Threading::MutexLocker lock(m_task_queue_mutex);
        m_task_queue.append(move(task));
        m_task_queue_is_empty = false;
    }

    AK::Duration last_sample_time() const
    {
        return AK::Duration::from_milliseconds(m_last_sample_time.load());
    }

private:
    AudioState(PlaybackStream::AudioDataRequestCallback data_request_callback, OutputState initial_output_state)
        : m_paused(initial_output_state == OutputState::Playing ? Paused::No : Paused::Yes)
        , m_data_request_callback(move(data_request_callback))
    {
    }

    Optional<AudioTask> dequeue_task()
    {
        // OPTIMIZATION: We can avoid taking a lock in the audio decoder thread if there are no queued commands, which
        //               will be the case most of the time.
        if (m_task_queue_is_empty.load())
            return {};

        Threading::MutexLocker lock(m_task_queue_mutex);

        m_task_queue_is_empty = m_task_queue.size() == 1;
        return m_task_queue.take_first();
    }

    static OSStatus on_audio_unit_buffer_request(void* user_data, AudioUnitRenderActionFlags*, AudioTimeStamp const* time_stamp, UInt32 element, UInt32 frames_to_render, AudioBufferList* output_buffer_list)
    {
        VERIFY(element == AUDIO_UNIT_OUTPUT_BUS);
        VERIFY(output_buffer_list->mNumberBuffers == 1);

        auto& state = *static_cast<AudioState*>(user_data);
        VERIFY(state.m_sample_specification.is_valid());

        VERIFY(time_stamp->mFlags & kAudioTimeStampSampleTimeValid);
        auto sample_time_seconds = time_stamp->mSampleTime / state.m_sample_specification.sample_rate();

        auto last_sample_time = static_cast<i64>(sample_time_seconds * 1000.0);
        state.m_last_sample_time.store(last_sample_time);

        if (auto task = state.dequeue_task(); task.has_value()) {
            OSStatus error = noErr;

            switch (task->type) {
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
                VERIFY(task->data.has_value());
                error = AudioUnitSetParameter(state.m_audio_unit, kHALOutputParam_Volume, kAudioUnitScope_Global, 0, static_cast<float>(*task->data), 0);
                break;
            }

            if (error == noErr)
                task->resolve(AK::Duration::from_milliseconds(last_sample_time));
            else
                task->reject(error);
        }

        auto& raw_buffer = output_buffer_list->mBuffers[0];
        auto output_buffer = Bytes(reinterpret_cast<u8*>(raw_buffer.mData), raw_buffer.mDataByteSize).reinterpret<float>();
        output_buffer = output_buffer.trim(static_cast<size_t>(frames_to_render) * state.m_sample_specification.channel_count());

        if (state.m_paused == Paused::No) {
            auto written_buffer = state.m_data_request_callback(output_buffer);

            if (written_buffer.is_empty())
                state.m_paused = Paused::Yes;
        }

        if (state.m_paused == Paused::Yes)
            output_buffer.fill(0);

        return noErr;
    }

    AudioComponentInstance m_audio_unit { nullptr };
    SampleSpecification m_sample_specification;

    Threading::Mutex m_task_queue_mutex;
    Vector<AudioTask, 4> m_task_queue;
    Atomic<bool> m_task_queue_is_empty { true };

    enum class Paused {
        Yes,
        No,
    };
    Paused m_paused { Paused::Yes };

    PlaybackStream::AudioDataRequestCallback m_data_request_callback;
    Atomic<i64> m_last_sample_time { 0 };
};

ErrorOr<NonnullRefPtr<PlaybackStream>> PlaybackStream::create(OutputState initial_output_state, u32 target_latency_ms, SampleSpecificationCallback&& sample_specification_callback, AudioDataRequestCallback&& data_request_callback)
{
    return PlaybackStreamAudioUnit::create(initial_output_state, target_latency_ms, move(sample_specification_callback), move(data_request_callback));
}

ErrorOr<NonnullRefPtr<PlaybackStream>> PlaybackStreamAudioUnit::create(OutputState initial_output_state, u32, SampleSpecificationCallback&& sample_specification_callback, AudioDataRequestCallback&& data_request_callback)
{
    auto state = TRY(AudioState::create(move(sample_specification_callback), move(data_request_callback), initial_output_state));
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
    m_state->queue_task({ AudioTask::Type::Play, promise });

    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamAudioUnit::drain_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    m_state->queue_task({ AudioTask::Type::Pause, promise });

    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamAudioUnit::discard_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    m_state->queue_task({ AudioTask::Type::PauseAndDiscard, promise });

    return promise;
}

AK::Duration PlaybackStreamAudioUnit::total_time_played() const
{
    return m_state->last_sample_time();
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamAudioUnit::set_volume(double volume)
{
    auto promise = Core::ThreadedPromise<void>::create();
    m_state->queue_task({ AudioTask::Type::Volume, promise, volume });

    return promise;
}

// This must be kept in the order defined by AudioChannelBitmap.
#define ENUMERATE_CHANNEL_POSITIONS(C)          \
    C(Left, Channel::FrontLeft)                 \
    C(Right, Channel::FrontRight)               \
    C(Center, Channel::FrontCenter)             \
    C(LFEScreen, Channel::LowFrequency)         \
    C(LeftSurround, Channel::BackLeft)          \
    C(RightSurround, Channel::BackRight)        \
    C(LeftCenter, Channel::FrontLeftOfCenter)   \
    C(RightCenter, Channel::FrontRightOfCenter) \
    C(CenterSurround, Channel::BackCenter)      \
    C(LeftSurroundDirect, Channel::SideLeft)    \
    C(RightSurroundDirect, Channel::SideRight)  \
    C(TopCenterSurround, Channel::TopCenter)    \
    C(TopBackLeft, Channel::TopBackLeft)        \
    C(TopBackCenter, Channel::TopBackCenter)    \
    C(TopBackRight, Channel::TopBackRight)      \
    C(LeftTopFront, Channel::TopFrontLeft)      \
    C(CenterTopFront, Channel::TopFrontCenter)  \
    C(RightTopFront, Channel::TopFrontRight)

ErrorOr<ChannelMap> audio_channel_layout_to_channel_map(AudioChannelLayout const& channel_layout)
{
    if (channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_Mono)
        return ChannelMap::mono();
    if (channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_Stereo
        || channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_StereoHeadphones)
        return ChannelMap::stereo();
    if (channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_Quadraphonic)
        return ChannelMap::quadrophonic();
    if (channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_AudioUnit_5_1)
        return ChannelMap::surround_5_1();
    if (channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_AudioUnit_7_1)
        return ChannelMap::surround_7_1();

    Vector<Channel, ChannelMap::capacity()> channels;

#define MAYBE_ADD_CHANNEL_FROM_BITMAP_FLAG(core_audio_channel_name, audio_channel)            \
    if ((channel_layout.mChannelBitmap & kAudioChannelBit_##core_audio_channel_name) != 0) {  \
        if (channels.size() == ChannelMap::capacity())                                        \
            return Error::from_string_literal("Device channel layout had too many channels"); \
        channels.unchecked_append(audio_channel);                                             \
    }

#define MAYBE_ADD_CHANNEL_FROM_CHANNEL_DESCRIPTION(core_audio_channel_name, audio_channel) \
    case kAudioChannelLabel_##core_audio_channel_name:                                     \
        channels.unchecked_append(audio_channel);                                          \
        break;

    if (channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap) {
        ENUMERATE_CHANNEL_POSITIONS(MAYBE_ADD_CHANNEL_FROM_BITMAP_FLAG);
    } else {
        auto fill_channels_from_channel_descriptions = [&](AudioChannelLayout const& channel_layout) {
            VERIFY(channel_layout.mNumberChannelDescriptions > 0);
            auto const* channel_descriptions = &channel_layout.mChannelDescriptions[0];
            for (u32 i = 0; i < channel_layout.mNumberChannelDescriptions; i++) {
                switch (channel_descriptions[i].mChannelLabel) {
                    ENUMERATE_CHANNEL_POSITIONS(MAYBE_ADD_CHANNEL_FROM_CHANNEL_DESCRIPTION)

                default:
                    channels.unchecked_append(Channel::Unknown);
                    break;
                }
            }
        };

        if (channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions) {
            fill_channels_from_channel_descriptions(channel_layout);
        } else {
            u32 explicit_layout_size = 0;
            AU_TRY(AudioFormatGetPropertyInfo(
                kAudioFormatProperty_ChannelLayoutForTag,
                sizeof(AudioChannelLayoutTag),
                &channel_layout.mChannelLayoutTag,
                &explicit_layout_size));
            VERIFY(explicit_layout_size >= sizeof(AudioChannelLayout));

            auto* explicit_layout = reinterpret_cast<AudioChannelLayout*>(malloc(explicit_layout_size));
            ScopeGuard free_explicit_layout { [&] { free(explicit_layout); } };

            AU_TRY(AudioFormatGetProperty(
                kAudioFormatProperty_ChannelLayoutForTag,
                sizeof(AudioChannelLayoutTag),
                &channel_layout.mChannelLayoutTag,
                &explicit_layout_size,
                explicit_layout));
            check_audio_channel_layout_size(*explicit_layout, explicit_layout_size);
            fill_channels_from_channel_descriptions(*explicit_layout);
        }
    }

    return ChannelMap(channels);
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
