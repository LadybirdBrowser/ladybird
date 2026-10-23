/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/GenericShorthands.h>
#include <AK/NeverDestroyed.h>
#include <AK/NumericLimits.h>
#include <AK/ScopeGuard.h>
#include <LibAudio/WindowsChannelMask.h>
#include <LibMedia/WindowsMediaFoundation/WindowsMediaFoundationAudioDecoder.h>
#include <LibMediaCodecs/AudioBlock.h>
#include <LibMediaCodecs/Codecs/AAC.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundation.h>

namespace Media::WindowsMediaFoundation {

struct WindowsMediaFoundationAudioDecoder::Transform {
    AK_ALLOC_WITH_KMALLOC;

    ~Transform()
    {
        if (output_stream_info.has_value())
            transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }

    ComPtr<IMFTransform> transform;
    // Absent until the transform has accepted an output type.
    Optional<OutputStreamInfo> output_stream_info;
    Audio::SampleSpecification output_sample_specification;
    bool outputs_float { true };

    // A packet the transform refused until it has output what it is holding.
    ComPtr<IMFSample> pending_input;
    bool drain_requested { false };
    bool next_input_is_discontinuous { false };
    Vector<AK::Duration, 4> pending_coded_frame_timestamps;
};

namespace {

bool aac_audio_object_type_is_supported(ParsedCodec const& codec)
{
    auto parameters = codec.aac_parameters();
    if (!parameters.has_value())
        return true;
    if (parameters->object_type_indication == Codecs::AAC::MPEG2_LOW_COMPLEXITY_OBJECT_TYPE_INDICATION)
        return true;
    if (parameters->object_type_indication != Codecs::AAC::MPEG4_AUDIO_OBJECT_TYPE_INDICATION)
        return false;
    if (!parameters->audio_object_type.has_value())
        return true;
    return first_is_one_of(*parameters->audio_object_type,
        Codecs::AAC::LOW_COMPLEXITY_AUDIO_OBJECT_TYPE,
        Codecs::AAC::SPECTRAL_BAND_REPLICATION_AUDIO_OBJECT_TYPE,
        Codecs::AAC::PARAMETRIC_STEREO_AUDIO_OBJECT_TYPE);
}

DecoderErrorOr<ComPtr<IMFMediaType>> create_aac_input_type(Audio::SampleSpecification const& container_sample_specification, ReadonlyBytes audio_specific_config)
{
    // The transform takes the fields of HEAACWAVEINFO that follow its WAVEFORMATEX, then the AudioSpecificConfig. A
    // payload type of zero says that packets are raw access units.
    static constexpr u16 NO_AUDIO_PROFILE_SPECIFIED = 0xFE;
    Vector<u8, 64> user_data;
    user_data.resize(12);
    user_data[2] = NO_AUDIO_PROFILE_SPECIFIED;
    DECODER_TRY_ALLOC(user_data.try_append(audio_specific_config.data(), audio_specific_config.size()));

    ComPtr<IMFMediaType> type;
    if (FAILED(MFCreateMediaType(&type))
        || FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio))
        || FAILED(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC))
        || FAILED(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, container_sample_specification.sample_rate()))
        || FAILED(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, container_sample_specification.channel_count()))
        || FAILED(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16))
        || FAILED(type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0))
        || FAILED(type->SetBlob(MF_MT_USER_DATA, user_data.data(), user_data.size())))
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to create a Media Foundation media type"sv);
    return type;
}

DecoderErrorOr<Audio::ChannelMap> output_channel_map(IMFMediaType& output_type, u32 channel_count)
{
    UINT32 channel_mask = 0;
    if (FAILED(output_type.GetUINT32(MF_MT_AUDIO_CHANNEL_MASK, &channel_mask))) {
        if (channel_count == 1)
            return Audio::ChannelMap::mono();
        if (channel_count == 2)
            return Audio::ChannelMap::stereo();
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Media Foundation output {} channels without saying where they go", channel_count);
    }

    auto channel_map_or_error = Audio::windows_channel_mask_to_channel_map(channel_mask);
    if (channel_map_or_error.is_error())
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Media Foundation output an unsupported channel layout: {}", channel_map_or_error.error());
    auto channel_map = channel_map_or_error.release_value();
    if (channel_map.channel_count() != channel_count)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Media Foundation output a channel layout that does not match its channel count"sv);
    return channel_map;
}

DecoderErrorOr<void> negotiate_output_format(WindowsMediaFoundationAudioDecoder::Transform& transform)
{
    auto& media_transform = *transform.transform.Get();
    auto output_type_or_error = set_first_acceptable_output_type(media_transform, [](GUID const& subtype) { return subtype == MFAudioFormat_Float; });
    if (output_type_or_error.is_error()) {
        output_type_or_error = set_first_acceptable_output_type(media_transform, [](GUID const& subtype) { return subtype == MFAudioFormat_PCM; });
        if (output_type_or_error.is_error())
            return output_type_or_error.release_error();
    }
    auto output_type = output_type_or_error.release_value();

    GUID subtype = GUID_NULL;
    UINT32 bits_per_sample = 0;
    UINT32 sample_rate = 0;
    UINT32 channel_count = 0;
    output_type->GetGUID(MF_MT_SUBTYPE, &subtype);
    output_type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits_per_sample);
    if (FAILED(output_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sample_rate)) || sample_rate == 0)
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Media Foundation output no sample rate"sv);
    if (FAILED(output_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channel_count)) || channel_count == 0 || channel_count > Audio::ChannelMap::capacity())
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Media Foundation cannot output {} channels", channel_count);

    auto outputs_float = subtype == MFAudioFormat_Float;
    if (bits_per_sample != (outputs_float ? 32 : 16))
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Media Foundation output {}-bit samples", bits_per_sample);

    auto channel_map = TRY(output_channel_map(*output_type.Get(), channel_count));
    auto stream_info = TRY(output_stream_info(media_transform));

    auto was_streaming = transform.output_stream_info.has_value();
    transform.output_stream_info = stream_info;
    transform.output_sample_specification = Audio::SampleSpecification { sample_rate, channel_map };
    transform.outputs_float = outputs_float;
    if (!was_streaming && FAILED(media_transform.ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)))
        return DecoderError::with_description(DecoderErrorCategory::Unknown, "Media Foundation decoder refused to start streaming"sv);
    return {};
}

DecoderErrorOr<void> submit_input(WindowsMediaFoundationAudioDecoder::Transform& transform, ComPtr<IMFSample> sample)
{
    auto result = transform.transform->ProcessInput(0, sample.Get(), 0);
    if (result == MF_E_NOTACCEPTING) {
        transform.pending_input = move(sample);
        return {};
    }
    if (FAILED(result))
        return DecoderError::format(DecoderErrorCategory::Corrupted, "Media Foundation decoder rejected a packet with result {:#x}", static_cast<u32>(result));
    return {};
}

void drain_unless_input_is_pending(WindowsMediaFoundationAudioDecoder::Transform& transform)
{
    if (transform.pending_input || !transform.drain_requested)
        return;
    transform.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    transform.transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
}

void request_drain(WindowsMediaFoundationAudioDecoder::Transform& transform)
{
    transform.drain_requested = true;
    drain_unless_input_is_pending(transform);
}

}

Optional<DecoderCapabilities> WindowsMediaFoundationAudioDecoder::capabilities(ParsedCodec const& codec)
{
    if (codec.codec_id() != CodecID::AAC || !aac_audio_object_type_is_supported(codec))
        return {};

    // Activating a transform to answer this costs enough that the answer is worth keeping.
    static bool const transform_is_available = [] {
        Audio::SampleSpecification sample_specification { 48'000, Audio::ChannelMap::stereo() };
        auto configuration = Codecs::AAC::create_configuration_record(Codecs::AAC::LOW_COMPLEXITY_AUDIO_OBJECT_TYPE, sample_specification.sample_rate(), sample_specification.channel_count());
        if (configuration.is_error())
            return false;
        auto input_type = create_aac_input_type(sample_specification, configuration.value().span());
        if (input_type.is_error())
            return false;
        return !create_decoder_transform(CodecID::AAC, *input_type.value().Get()).is_error();
    }();
    if (!transform_is_available)
        return {};
    return DecoderCapabilities { .smooth = true, .power_efficient = true };
}

DecoderErrorOr<NonnullOwnPtr<WindowsMediaFoundationAudioDecoder>> WindowsMediaFoundationAudioDecoder::try_create(CodecID codec_id, Audio::SampleSpecification const& sample_specification, ReadonlyBytes codec_initialization_data)
{
    if (codec_id != CodecID::AAC)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Media Foundation has no audio decoder for codec {}", codec_id);

    auto codec_configuration = DECODER_TRY_ALLOC(FixedArray<u8>::create(codec_initialization_data));
    return DECODER_TRY_ALLOC(adopt_nonnull_own_or_enomem(new (nothrow) WindowsMediaFoundationAudioDecoder(codec_id, sample_specification, move(codec_configuration))));
}

WindowsMediaFoundationAudioDecoder::WindowsMediaFoundationAudioDecoder(CodecID codec_id, Audio::SampleSpecification const& sample_specification, FixedArray<u8>&& codec_configuration)
    : m_codec_id(codec_id)
    , m_container_sample_specification(sample_specification)
    , m_codec_configuration(move(codec_configuration))
{
}

WindowsMediaFoundationAudioDecoder::~WindowsMediaFoundationAudioDecoder() = default;

DecoderErrorOr<void> WindowsMediaFoundationAudioDecoder::ensure_transform_for_frame(CodedFrame const& coded_frame)
{
    auto new_configuration = coded_frame.new_codec_configuration();
    auto configuration_changed = new_configuration.has_value() && !new_configuration->is_empty() && *new_configuration != m_codec_configuration.span();
    if (m_transform != nullptr && !configuration_changed)
        return {};

    auto configuration = configuration_changed ? *new_configuration : m_codec_configuration.span();
    if (configuration.is_empty())
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Media Foundation cannot decode AAC without an Audio Specific Config"sv);

    auto input_type = TRY(create_aac_input_type(m_container_sample_specification, configuration));
    auto transform = DECODER_TRY_ALLOC(adopt_nonnull_own_or_enomem(new (nothrow) Transform));
    transform->transform = TRY(create_decoder_transform(m_codec_id, *input_type.Get()));
    // Some decoders refuse every output format until they have seen input, so this is retried when they ask.
    (void)negotiate_output_format(*transform);

    if (configuration_changed)
        m_codec_configuration = DECODER_TRY_ALLOC(FixedArray<u8>::create(configuration));

    if (m_transform == nullptr) {
        m_transform = move(transform);
        return {};
    }
    request_drain(*m_transform);
    m_replacement_transform = move(transform);
    return {};
}

DecoderErrorOr<void> WindowsMediaFoundationAudioDecoder::receive_coded_data(CodedFrame const& coded_frame)
{
    if (m_end_of_stream_signaled)
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "Media Foundation decoder has been drained"sv);
    if (m_replacement_transform != nullptr || (m_transform != nullptr && m_transform->pending_input))
        return DecoderError::with_description(DecoderErrorCategory::TryAgain, "Media Foundation decoder has not taken the previous packet yet"sv);
    if (coded_frame.data().size() > NumericLimits<DWORD>::max())
        return DecoderError::corrupted("Coded frame is too large for Media Foundation"sv);

    TRY(ensure_transform_for_frame(coded_frame));

    auto& transform = m_replacement_transform != nullptr ? *m_replacement_transform : *m_transform;
    auto sample = TRY(create_input_sample(coded_frame.data(), coded_frame.decoded_start_timestamp(), coded_frame.duration() + coded_frame.leading_discard() + coded_frame.trailing_discard()));
    if (transform.next_input_is_discontinuous)
        sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
    TRY(submit_input(transform, move(sample)));
    transform.next_input_is_discontinuous = false;
    DECODER_TRY_ALLOC(transform.pending_coded_frame_timestamps.try_append(coded_frame.decoded_start_timestamp()));
    m_discard_intervals.add_intervals_for_frame(coded_frame);
    return {};
}

void WindowsMediaFoundationAudioDecoder::signal_end_of_stream()
{
    m_end_of_stream_signaled = true;
    if (m_replacement_transform != nullptr)
        request_drain(*m_replacement_transform);
    else if (m_transform != nullptr)
        request_drain(*m_transform);
}

void WindowsMediaFoundationAudioDecoder::switch_to_replacement_transform()
{
    m_transform = m_replacement_transform.release_nonnull();
}

DecoderErrorOr<void> WindowsMediaFoundationAudioDecoder::copy_output_sample(Transform& transform, IMFSample& sample)
{
    // The sample is copied, since a block may take only part of it.
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample.ConvertToContiguousBuffer(&buffer)))
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to read a Media Foundation output sample"sv);
    BYTE* data = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &length)))
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to lock a Media Foundation output buffer"sv);
    ScopeGuard unlock_buffer = [&] { buffer->Unlock(); };

    auto const& sample_specification = transform.output_sample_specification;
    auto sample_size = transform.outputs_float ? sizeof(float) : sizeof(i16);
    auto frame_count = length / (sample_size * sample_specification.channel_count());
    auto sample_count = frame_count * sample_specification.channel_count();

    Optional<AK::Duration> previous_output_end;
    if (m_output_timeline_start.has_value())
        previous_output_end = *m_output_timeline_start + AK::Duration::from_time_units(m_frames_output_on_timeline, 1, m_output_sample_specification.sample_rate());
    auto output_start = previous_output_end;
    LONGLONG sample_time = 0;
    if (SUCCEEDED(sample.GetSampleTime(&sample_time)))
        output_start = duration_from_sample_time(sample_time);
    if (!output_start.has_value())
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Media Foundation decoder output a sample with no time"sv);

    // The MFT output's time can be slightly off our input time in both directions, so stamp our output with the latest
    // time before the MFT output's midpoint. Anything before that time is dropped, since the output is monotonic.
    auto output_midpoint = *output_start + AK::Duration::from_time_units(static_cast<i64>(frame_count), 1, sample_specification.sample_rate() * 2);
    Optional<AK::Duration> packet_start;
    while (!transform.pending_coded_frame_timestamps.is_empty() && transform.pending_coded_frame_timestamps.first() <= output_midpoint)
        packet_start = transform.pending_coded_frame_timestamps.take_first();

    if (packet_start.has_value()) {
        m_output_timeline_start = packet_start.release_value();
        m_frames_output_on_timeline = 0;
    } else if (!previous_output_end.has_value()) {
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Media Foundation decoder produced a sample that matches no packet"sv);
    } else if (m_output_sample_specification.sample_rate() != sample_specification.sample_rate()) {
        m_output_timeline_start = previous_output_end;
        m_frames_output_on_timeline = 0;
    }

    m_output_samples.clear_with_capacity();
    DECODER_TRY_ALLOC(m_output_samples.try_resize(sample_count));
    if (transform.outputs_float) {
        memcpy(m_output_samples.data(), data, sample_count * sizeof(float));
    } else {
        for (size_t index = 0; index < sample_count; index++) {
            i16 value;
            memcpy(&value, data + (index * sizeof(i16)), sizeof(value));
            m_output_samples[index] = static_cast<float>(value) / 32768.0f;
        }
    }

    m_output_sample_specification = sample_specification;
    m_output_frames_written = 0;
    return {};
}

void WindowsMediaFoundationAudioDecoder::write_output_into_block(AudioBlock& block)
{
    auto const& sample_specification = m_output_sample_specification;
    auto channel_count = sample_specification.channel_count();
    auto sample_rate = sample_specification.sample_rate();
    auto timestamp = *m_output_timeline_start + AK::Duration::from_time_units(m_frames_output_on_timeline, 1, sample_rate);

    auto available_frame_count = (m_output_samples.size() / channel_count) - m_output_frames_written;
    auto frame_count = min(available_frame_count, AudioBlock::max_frame_count(channel_count));
    frame_count = m_discard_intervals.frames_until_next_boundary(timestamp, sample_rate, frame_count);
    block.initialize(sample_specification, timestamp, frame_count);

    for (u8 channel = 0; channel < channel_count; channel++) {
        auto channel_data = block.channel_data(channel);
        for (size_t frame = 0; frame < frame_count; frame++)
            channel_data[frame] = m_output_samples[((m_output_frames_written + frame) * channel_count) + channel];
    }
    m_output_frames_written += frame_count;
    m_frames_output_on_timeline += static_cast<i64>(frame_count);
}

DecoderErrorOr<void> WindowsMediaFoundationAudioDecoder::write_next_block(AudioBlock& block)
{
    while (m_transform != nullptr) {
        auto has_unwritten_output = m_output_sample_specification.is_valid() && m_output_frames_written < m_output_samples.size() / m_output_sample_specification.channel_count();
        if (has_unwritten_output) {
            write_output_into_block(block);
            if (m_discard_intervals.should_discard(block))
                continue;
            return {};
        }

        auto& transform = *m_transform;
        auto output = TRY(process_output(*transform.transform.Get(), transform.output_stream_info));
        if (output.has<ComPtr<IMFSample>>()) {
            TRY(copy_output_sample(transform, *output.get<ComPtr<IMFSample>>().Get()));
            continue;
        }
        if (output.has<OutputTypeNeeded>()) {
            TRY(negotiate_output_format(transform));
            continue;
        }

        if (transform.pending_input) {
            TRY(submit_input(transform, move(transform.pending_input)));
            drain_unless_input_is_pending(transform);
            continue;
        }
        // A transform that was asked to drain has now output everything it withheld.
        if (transform.drain_requested && m_replacement_transform != nullptr) {
            switch_to_replacement_transform();
            continue;
        }
        break;
    }

    if (m_end_of_stream_signaled)
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "Media Foundation decoder has been drained"sv);
    return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "Media Foundation decoder needs more input"sv);
}

void WindowsMediaFoundationAudioDecoder::flush()
{
    if (m_replacement_transform != nullptr)
        switch_to_replacement_transform();
    if (m_transform != nullptr) {
        m_transform->transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        m_transform->pending_input = nullptr;
        m_transform->drain_requested = false;
        m_transform->next_input_is_discontinuous = true;
        m_transform->pending_coded_frame_timestamps.clear();
    }

    m_end_of_stream_signaled = false;
    m_output_samples.clear_with_capacity();
    m_output_frames_written = 0;
    m_output_timeline_start.clear();
    m_frames_output_on_timeline = 0;
    m_discard_intervals.clear();
}

}
