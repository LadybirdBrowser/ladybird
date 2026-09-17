/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/NumericLimits.h>
#include <AK/Vector.h>
#include <LibMedia/Audio/CoreAudioChannelLayout.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/AudioToolbox/AudioToolboxAudioDecoder.h>
#include <LibMedia/Codecs/AAC.h>
#include <LibMedia/CodedFrame.h>

#include <AudioToolbox/AudioConverter.h>
#include <AudioToolbox/AudioFormat.h>

namespace Media::AudioToolbox {

struct AudioToolboxAudioDecoder::Converter {
    ~Converter()
    {
        if (converter != nullptr)
            AudioConverterDispose(converter);
    }

    AudioConverterRef converter { nullptr };
    Audio::SampleSpecification output_sample_specification;
    Optional<u32> frames_per_packet;
    // Frames the converter has taken input for but not output yet, such as those that SBR delays.
    i64 withheld_frame_count { 0 };
    AudioStreamPacketDescription packet_description {};
};

namespace {

// Our own unique result to be returned by the input callback to end a conversion without triggering EOS.
constexpr OSStatus CONVERTER_NEEDS_INPUT = 'moar';

// AudioBufferList ends in a single-element array that callers are expected to allocate past, so this reserves room
// for as many channels as a channel map can describe.
struct ChannelMapAudioBufferList {
    UInt32 buffer_count { 0 };
    Array<AudioBuffer, Audio::ChannelMap::capacity()> buffers;
};
static_assert(offsetof(ChannelMapAudioBufferList, buffer_count) == offsetof(AudioBufferList, mNumberBuffers));
static_assert(offsetof(ChannelMapAudioBufferList, buffers) == offsetof(AudioBufferList, mBuffers));

struct InputFormat {
    AudioStreamBasicDescription description;
    FixedArray<u8> magic_cookie;
};

Optional<AudioFormatID> format_id_for_aac(Optional<Codecs::AAC::Parameters const&> parameters)
{
    if (!parameters.has_value())
        return kAudioFormatMPEG4AAC;
    if (parameters->object_type_indication == Codecs::AAC::MPEG2_LOW_COMPLEXITY_OBJECT_TYPE_INDICATION)
        return kAudioFormatMPEG4AAC;
    if (parameters->object_type_indication != Codecs::AAC::MPEG4_AUDIO_OBJECT_TYPE_INDICATION)
        return {};
    if (!parameters->audio_object_type.has_value())
        return kAudioFormatMPEG4AAC;

    switch (*parameters->audio_object_type) {
    case Codecs::AAC::LOW_COMPLEXITY_AUDIO_OBJECT_TYPE:
        return kAudioFormatMPEG4AAC;
    case Codecs::AAC::SPECTRAL_BAND_REPLICATION_AUDIO_OBJECT_TYPE:
        return kAudioFormatMPEG4AAC_HE;
    case Codecs::AAC::LOW_DELAY_AUDIO_OBJECT_TYPE:
        return kAudioFormatMPEG4AAC_LD;
    case Codecs::AAC::PARAMETRIC_STEREO_AUDIO_OBJECT_TYPE:
        return kAudioFormatMPEG4AAC_HE_V2;
    case Codecs::AAC::ENHANCED_LOW_DELAY_AUDIO_OBJECT_TYPE:
        return kAudioFormatMPEG4AAC_ELD;
    default:
        return {};
    }
}

Optional<AudioFormatID> format_id_for_codec(ParsedCodec const& codec)
{
    switch (codec.codec_id()) {
    case CodecID::AAC:
        return format_id_for_aac(codec.aac_parameters());
    default:
        return {};
    }
}

bool platform_has_decoder_for_format(AudioFormatID format_id)
{
    UInt32 decoders_size = 0;
    if (AudioFormatGetPropertyInfo(kAudioFormatProperty_Decoders, sizeof(format_id), &format_id, &decoders_size) != noErr)
        return false;
    return decoders_size > 0;
}

DecoderErrorOr<InputFormat> input_format_for_codec(CodecID codec_id, Audio::SampleSpecification const& container_sample_specification, ReadonlyBytes codec_configuration)
{
    InputFormat format {};
    format.description.mSampleRate = container_sample_specification.sample_rate();
    format.description.mChannelsPerFrame = container_sample_specification.channel_count();

    switch (codec_id) {
    case CodecID::AAC:
        if (codec_configuration.is_empty())
            return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "AudioToolbox cannot decode AAC without an Audio Specific Config"sv);
        format.description.mFormatID = kAudioFormatMPEG4AAC;
        format.magic_cookie = TRY(Codecs::AAC::elementary_stream_descriptor_for_configuration_record(codec_configuration));
        return format;
    default:
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "AudioToolbox has no decoder for codec {}", codec_id);
    }
}

DecoderErrorOr<AudioStreamBasicDescription> resolve_input_description(InputFormat const& format)
{
    auto description = format.description;
    if (format.magic_cookie.is_empty())
        return description;

    UInt32 description_size = sizeof(description);
    if (auto status = AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, format.magic_cookie.size(), format.magic_cookie.data(), &description_size, &description); status != noErr)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "AudioToolbox could not read the codec configuration, status {}", status);

    // A layered format such as HE-AAC lists every layer it can be decoded as, highest quality first.
    AudioFormatInfo format_info {};
    format_info.mASBD = description;
    format_info.mMagicCookie = format.magic_cookie.data();
    format_info.mMagicCookieSize = format.magic_cookie.size();

    UInt32 list_size = 0;
    if (AudioFormatGetPropertyInfo(kAudioFormatProperty_FormatList, sizeof(format_info), &format_info, &list_size) != noErr)
        return description;
    Vector<AudioFormatListItem, 4> layers;
    layers.resize(list_size / sizeof(AudioFormatListItem));
    if (AudioFormatGetProperty(kAudioFormatProperty_FormatList, sizeof(format_info), &format_info, &list_size, layers.data()) != noErr)
        return description;
    layers.shrink(list_size / sizeof(AudioFormatListItem));

    for (auto const& layer : layers) {
        if (platform_has_decoder_for_format(layer.mASBD.mFormatID))
            return layer.mASBD;
    }
    return description;
}

AudioStreamBasicDescription planar_float_description(Float64 sample_rate, UInt32 channel_count)
{
    AudioStreamBasicDescription description {};
    description.mSampleRate = sample_rate;
    description.mFormatID = kAudioFormatLinearPCM;
    description.mFormatFlags = static_cast<AudioFormatFlags>(kAudioFormatFlagsNativeFloatPacked) | kAudioFormatFlagIsNonInterleaved;
    description.mBytesPerPacket = sizeof(float);
    description.mFramesPerPacket = 1;
    description.mBytesPerFrame = sizeof(float);
    description.mChannelsPerFrame = channel_count;
    description.mBitsPerChannel = 8 * sizeof(float);
    return description;
}

DecoderErrorOr<Audio::ChannelMap> output_channel_map(AudioConverterRef converter, u32 channel_count)
{
    UInt32 layout_size = 0;
    if (auto status = AudioConverterGetPropertyInfo(converter, kAudioConverterOutputChannelLayout, &layout_size, nullptr); status != noErr)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "AudioToolbox did not describe its output channel layout, status {}", status);
    if (layout_size < offsetof(AudioChannelLayout, mChannelDescriptions))
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "AudioToolbox described its output channel layout incompletely"sv);

    // Word-sized storage keeps the layout aligned for its fields.
    Vector<u32, 16> layout_storage;
    layout_storage.resize(ceil_div(max<size_t>(layout_size, sizeof(AudioChannelLayout)), sizeof(u32)));
    auto* layout = reinterpret_cast<AudioChannelLayout*>(layout_storage.data());
    if (auto status = AudioConverterGetProperty(converter, kAudioConverterOutputChannelLayout, &layout_size, layout); status != noErr)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "AudioToolbox did not describe its output channel layout, status {}", status);
    auto channel_map_or_error = Audio::core_audio_channel_layout_to_channel_map(*layout, layout_size);
    if (channel_map_or_error.is_error())
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "AudioToolbox output an unsupported channel layout: {}", channel_map_or_error.error());
    auto channel_map = channel_map_or_error.release_value();
    if (channel_map.channel_count() != channel_count)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "AudioToolbox output a channel layout that does not match its channel count"sv);
    return channel_map;
}

}

Optional<DecoderCapabilities> AudioToolboxAudioDecoder::capabilities(ParsedCodec const& codec)
{
    auto format_id = format_id_for_codec(codec);
    if (!format_id.has_value() || !platform_has_decoder_for_format(*format_id))
        return {};
    return DecoderCapabilities { .smooth = true, .power_efficient = true };
}

DecoderErrorOr<NonnullOwnPtr<AudioToolboxAudioDecoder>> AudioToolboxAudioDecoder::try_create(CodecID codec_id, Audio::SampleSpecification const& sample_specification, ReadonlyBytes codec_initialization_data)
{
    if (!format_id_for_codec(ParsedCodec { codec_id }).has_value())
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "AudioToolbox has no decoder for codec {}", codec_id);

    auto codec_configuration = MUST(FixedArray<u8>::create(codec_initialization_data));
    return adopt_own(*new AudioToolboxAudioDecoder(codec_id, sample_specification, move(codec_configuration)));
}

AudioToolboxAudioDecoder::AudioToolboxAudioDecoder(CodecID codec_id, Audio::SampleSpecification const& sample_specification, FixedArray<u8>&& codec_configuration)
    : m_codec_id(codec_id)
    , m_container_sample_specification(sample_specification)
    , m_codec_configuration(move(codec_configuration))
{
}

AudioToolboxAudioDecoder::~AudioToolboxAudioDecoder() = default;

DecoderErrorOr<NonnullOwnPtr<AudioToolboxAudioDecoder::Converter>> AudioToolboxAudioDecoder::create_converter(CodecID codec_id, Audio::SampleSpecification const& container_sample_specification, ReadonlyBytes codec_configuration)
{
    auto format = TRY(input_format_for_codec(codec_id, container_sample_specification, codec_configuration));
    auto input_description = TRY(resolve_input_description(format));
    if (input_description.mChannelsPerFrame == 0 || input_description.mChannelsPerFrame > Audio::ChannelMap::capacity())
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "AudioToolbox cannot output {} channels", input_description.mChannelsPerFrame);
    if (input_description.mSampleRate < 1 || input_description.mSampleRate > NumericLimits<u32>::max())
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "AudioToolbox was given an invalid sample rate"sv);

    auto converter = adopt_own(*new Converter);

    auto output_description = planar_float_description(input_description.mSampleRate, input_description.mChannelsPerFrame);
    if (auto status = AudioConverterNew(&input_description, &output_description, &converter->converter); status != noErr)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "AudioToolbox has no decoder for this format, status {}", status);

    if (!format.magic_cookie.is_empty()) {
        if (auto status = AudioConverterSetProperty(converter->converter, kAudioConverterDecompressionMagicCookie, format.magic_cookie.size(), format.magic_cookie.data()); status != noErr)
            return DecoderError::format(DecoderErrorCategory::NotImplemented, "AudioToolbox rejected the codec configuration, status {}", status);
    }

    UInt32 description_size = sizeof(input_description);
    if (auto status = AudioConverterGetProperty(converter->converter, kAudioConverterCurrentInputStreamDescription, &description_size, &input_description); status != noErr)
        return DecoderError::format(DecoderErrorCategory::Unknown, "AudioToolbox did not describe its input format, status {}", status);
    description_size = sizeof(output_description);
    if (auto status = AudioConverterGetProperty(converter->converter, kAudioConverterCurrentOutputStreamDescription, &description_size, &output_description); status != noErr)
        return DecoderError::format(DecoderErrorCategory::Unknown, "AudioToolbox did not describe its output format, status {}", status);

    auto channel_map = TRY(output_channel_map(converter->converter, output_description.mChannelsPerFrame));
    converter->output_sample_specification = Audio::SampleSpecification { static_cast<u32>(output_description.mSampleRate), channel_map };
    if (input_description.mFramesPerPacket != 0)
        converter->frames_per_packet = input_description.mFramesPerPacket;
    return converter;
}

DecoderErrorOr<void> AudioToolboxAudioDecoder::ensure_converter_for_frame(CodedFrame const& coded_frame)
{
    auto new_configuration = coded_frame.new_codec_configuration();
    auto configuration_changed = new_configuration.has_value() && !new_configuration->is_empty() && *new_configuration != m_codec_configuration.span();
    if (m_converter != nullptr && !configuration_changed)
        return {};

    auto configuration = configuration_changed ? *new_configuration : m_codec_configuration.span();
    auto converter = TRY(create_converter(m_codec_id, m_container_sample_specification, configuration));
    if (configuration_changed)
        m_codec_configuration = MUST(FixedArray<u8>::create(configuration));

    if (m_converter == nullptr)
        m_converter = move(converter);
    else
        m_replacement_converter = move(converter);
    return {};
}

void AudioToolboxAudioDecoder::start_output_timeline_at_pending_packet()
{
    // Output that the converter is still withholding precedes this packet's own.
    auto sample_rate = m_converter->output_sample_specification.sample_rate();
    m_output_timeline_start = m_pending_packet_timestamp - AK::Duration::from_time_units(m_converter->withheld_frame_count, 1, sample_rate);
    m_frames_output_on_timeline = 0;
}

DecoderErrorOr<void> AudioToolboxAudioDecoder::receive_coded_data(CodedFrame const& coded_frame)
{
    if (m_end_of_stream_signaled)
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "AudioToolbox decoder has been drained"sv);
    if (m_has_pending_packet)
        return DecoderError::with_description(DecoderErrorCategory::TryAgain, "AudioToolbox decoder has not taken the previous packet yet"sv);
    if (coded_frame.data().size() > NumericLimits<UInt32>::max())
        return DecoderError::corrupted("Coded frame is too large for AudioToolbox"sv);

    TRY(ensure_converter_for_frame(coded_frame));

    m_pending_packet.clear();
    MUST(m_pending_packet.try_append(coded_frame.data()));
    m_pending_packet_timestamp = coded_frame.presentation_timestamp();
    m_has_pending_packet = true;
    m_converter_awaits_input = false;

    if (m_replacement_converter == nullptr)
        start_output_timeline_at_pending_packet();
    return {};
}

void AudioToolboxAudioDecoder::signal_end_of_stream()
{
    m_end_of_stream_signaled = true;
}

DecoderErrorOr<void> AudioToolboxAudioDecoder::convert_into_block(AudioBlock& block)
{
    auto supply_input = [](AudioConverterRef, UInt32* packet_count, AudioBufferList* buffers, AudioStreamPacketDescription** packet_descriptions, void* user_data) -> OSStatus {
        auto& decoder = *static_cast<AudioToolboxAudioDecoder*>(user_data);
        auto& converter = *decoder.m_converter;

        // A replacement converter is waiting for the pending packet, so this one only has to finish its output.
        if (decoder.m_has_pending_packet && decoder.m_replacement_converter == nullptr) {
            decoder.m_has_pending_packet = false;
            converter.withheld_frame_count += converter.frames_per_packet.value_or(0);

            auto packet = decoder.m_pending_packet.bytes();
            converter.packet_description = { .mStartOffset = 0, .mVariableFramesInPacket = 0, .mDataByteSize = static_cast<UInt32>(packet.size()) };
            *packet_count = 1;
            buffers->mBuffers[0].mData = packet.data();
            buffers->mBuffers[0].mDataByteSize = static_cast<UInt32>(packet.size());
            if (packet_descriptions != nullptr)
                *packet_descriptions = &converter.packet_description;
            return noErr;
        }

        *packet_count = 0;
        buffers->mBuffers[0].mData = nullptr;
        buffers->mBuffers[0].mDataByteSize = 0;
        if (decoder.m_end_of_stream_signaled || decoder.m_replacement_converter != nullptr)
            return noErr;
        return CONVERTER_NEEDS_INPUT;
    };

    auto& converter = *m_converter;
    auto const& sample_specification = converter.output_sample_specification;
    auto channel_count = sample_specification.channel_count();
    auto frame_capacity = AudioBlock::max_frame_count(channel_count);

    auto timestamp = m_output_timeline_start + AK::Duration::from_time_units(m_frames_output_on_timeline, 1, sample_specification.sample_rate());
    block.initialize(sample_specification, timestamp, frame_capacity);

    ChannelMapAudioBufferList buffer_list;
    buffer_list.buffer_count = channel_count;
    for (u8 channel = 0; channel < channel_count; channel++) {
        auto channel_data = block.channel_data(channel);
        buffer_list.buffers[channel] = { .mNumberChannels = 1, .mDataByteSize = static_cast<UInt32>(channel_data.size() * sizeof(float)), .mData = channel_data.data() };
    }

    auto frame_count = static_cast<UInt32>(frame_capacity);
    auto status = AudioConverterFillComplexBuffer(converter.converter, supply_input, this, &frame_count, reinterpret_cast<AudioBufferList*>(&buffer_list), nullptr);

    converter.withheld_frame_count = max<i64>(converter.withheld_frame_count - frame_count, 0);
    m_frames_output_on_timeline += frame_count;

    if (status == CONVERTER_NEEDS_INPUT)
        m_converter_awaits_input = true;
    else if (status != noErr)
        frame_count = 0;

    if (frame_count == 0)
        block.clear();
    else
        block.trim(frame_count);

    if (status != noErr && status != CONVERTER_NEEDS_INPUT)
        return DecoderError::format(DecoderErrorCategory::Corrupted, "AudioToolbox failed to decode a packet with status {}", status);
    return {};
}

DecoderErrorOr<void> AudioToolboxAudioDecoder::write_next_block(AudioBlock& block)
{
    while (m_converter != nullptr) {
        auto draining = m_end_of_stream_signaled || m_replacement_converter != nullptr;
        if (!draining && m_converter_awaits_input)
            break;

        TRY(convert_into_block(block));
        if (!block.is_empty())
            return {};
        if (!draining || m_replacement_converter == nullptr)
            break;

        m_converter = m_replacement_converter.release_nonnull();
        start_output_timeline_at_pending_packet();
    }

    if (m_end_of_stream_signaled)
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "AudioToolbox decoder has been drained"sv);
    return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "AudioToolbox decoder needs more input"sv);
}

void AudioToolboxAudioDecoder::flush()
{
    if (m_replacement_converter != nullptr)
        m_converter = m_replacement_converter.release_nonnull();
    if (m_converter != nullptr) {
        AudioConverterReset(m_converter->converter);
        m_converter->withheld_frame_count = 0;
    }

    m_has_pending_packet = false;
    m_converter_awaits_input = true;
    m_end_of_stream_signaled = false;
    m_frames_output_on_timeline = 0;
}

}
