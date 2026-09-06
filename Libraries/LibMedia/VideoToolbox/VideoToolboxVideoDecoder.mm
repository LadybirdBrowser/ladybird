/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibMedia/Codecs/AV1.h>
#include <LibMedia/Codecs/H264.h>
#include <LibMedia/Codecs/NALUnit.h>
#include <LibMedia/Codecs/VP9.h>
#include <LibMedia/CodedFrame.h>
#include <LibMedia/VideoFrame.h>
#include <LibMedia/VideoToolbox/VideoToolboxVideoDecoder.h>

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dispatch/dispatch.h>

namespace Media::VideoToolbox {

struct VideoToolboxVideoDecoder::Session {
    explicit Session(VideoToolboxVideoDecoder& decoder)
        : decoder(decoder)
    {
    }

    ~Session()
    {
        if (session) {
            VTDecompressionSessionWaitForAsynchronousFrames(session);
            auto* drained_session = session;
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                VTDecompressionSessionInvalidate(drained_session);
                CFRelease(drained_session);
            });
        }
        if (format_description)
            CFRelease(format_description);
    }

    VideoToolboxVideoDecoder& decoder;
    VTDecompressionSessionRef session { nullptr };
    CMVideoFormatDescriptionRef format_description { nullptr };
    CodingIndependentCodePoints cicp;
};

namespace {

template<typename T>
class RetainedRef {
public:
    RetainedRef() = default;
    explicit RetainedRef(T ref)
        : m_ref(ref)
    {
    }
    RetainedRef(RetainedRef const&) = delete;
    RetainedRef& operator=(RetainedRef const&) = delete;
    RetainedRef(RetainedRef&& other)
        : m_ref(exchange(other.m_ref, nullptr))
    {
    }
    ~RetainedRef()
    {
        if (m_ref)
            CFRelease(m_ref);
    }

    T ref() const { return m_ref; }
    T* out() { return &m_ref; }

private:
    T m_ref { nullptr };
};

// How many frames the media engine may be decoding before it has to hand one back.
constexpr size_t MAXIMUM_FRAMES_IN_FLIGHT = 4;

AK::Duration duration_from_cm_time(CMTime time)
{
    if (!CMTIME_IS_NUMERIC(time))
        return AK::Duration::zero();
    return AK::Duration::from_time_units(time.value, 1, static_cast<u32>(time.timescale));
}

CMTime cm_time_from_duration(AK::Duration duration)
{
    return CMTimeMake(duration.to_nanoseconds(), 1'000'000'000);
}

CMVideoCodecType codec_type_from_codec_id(CodecID codec_id)
{
    switch (codec_id) {
    case CodecID::VP9:
        return kCMVideoCodecType_VP9;
    case CodecID::AV1:
        return kCMVideoCodecType_AV1;
    case CodecID::H264:
        return kCMVideoCodecType_H264;
    default:
        return 0;
    }
}

// VP9 has no parameter set based constructor, so its format description carries the same codec configuration record
// a container would, built from what the bitstream itself said.
Array<u8, 12> vp9_configuration_record(u8 profile, u8 bit_depth, Codecs::VP9::ColorParameters const& color_parameters)
{
    auto const& cicp = color_parameters.cicp;
    auto subsampling = color_parameters.subsampling;
    u8 chroma_subsampling = subsampling.x() && subsampling.y() ? 1 : 3;
    u8 full_range = cicp.video_full_range_flag() == VideoFullRangeFlag::Full ? 1 : 0;

    return Array<u8, 12> {
        1, 0, 0, 0,
        profile,
        0,
        static_cast<u8>((bit_depth << 4) | (chroma_subsampling << 1) | full_range),
        static_cast<u8>(to_underlying(cicp.color_primaries())),
        static_cast<u8>(to_underlying(cicp.transfer_characteristics())),
        static_cast<u8>(to_underlying(cicp.matrix_coefficients())),
        0, 0
    };
}

// AV1's configuration record is the sequence header's own fields, minus the config OBUs that are optional in it and
// that the stream carries in band anyway.
// https://aomediacodec.github.io/av1-isobmff/#av1codecconfigurationbox-syntax
Array<u8, 4> av1_configuration_record(Codecs::AV1::Parameters const& parameters)
{
    auto const& optional_fields = parameters.optional_fields;
    u8 tier = parameters.tier == Codecs::AV1::Tier::High ? 1 : 0;
    u8 high_bitdepth = parameters.bit_depth > 8 ? 1 : 0;
    u8 twelve_bit = parameters.bit_depth == 12 ? 1 : 0;

    return Array<u8, 4> {
        0x81, // marker and version
        static_cast<u8>((parameters.profile << 5) | parameters.level),
        static_cast<u8>((tier << 7) | (high_bitdepth << 6) | (twelve_bit << 5) | (optional_fields.monochrome << 4)
            | (optional_fields.subsampling.x() << 3) | (optional_fields.subsampling.y() << 2) | optional_fields.chroma_sample_position),
        0,
    };
}

// How a stream's format is described to VideoToolbox, which is both what a session is created against and what a
// change in the stream is recognised by.
struct DecoderFormat {
    RetainedRef<CMVideoFormatDescriptionRef> description;
    RetainedRef<CFMutableDictionaryRef> specification;
    RetainedRef<CFMutableDictionaryRef> destination_attributes;
    CodingIndependentCodePoints cicp;
    u8 reorder_frame_count { 0 };
};

// Left to choose for itself, a decoder hands back layouts that pack samples across element boundaries, which nothing
// downstream can sample or read. Asking for the documented biplanar formats keeps the output to one sample per
// component, in whichever range the stream is in.
RetainedRef<CFMutableDictionaryRef> destination_attributes_for_bit_depth(u8 bit_depth)
{
    Array<OSType, 2> pixel_formats { kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange };
    if (bit_depth > 8)
        pixel_formats = { kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr10BiPlanarFullRange };

    RetainedRef<CFMutableArrayRef> pixel_format_numbers { CFArrayCreateMutable(kCFAllocatorDefault, pixel_formats.size(), &kCFTypeArrayCallBacks) };
    for (auto pixel_format : pixel_formats) {
        RetainedRef<CFNumberRef> number { CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format) };
        CFArrayAppendValue(pixel_format_numbers.ref(), number.ref());
    }

    RetainedRef<CFMutableDictionaryRef> attributes { CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) };
    CFDictionarySetValue(attributes.ref(), kCVPixelBufferPixelFormatTypeKey, pixel_format_numbers.ref());
    return attributes;
}

Optional<DecoderFormat> vp9_decoder_format(CMVideoCodecType codec_type, u8 profile, u8 bit_depth, Codecs::VP9::ColorParameters const& color_parameters, Gfx::IntSize size)
{
    auto configuration_record = vp9_configuration_record(profile, bit_depth, color_parameters);
    RetainedRef<CFDataRef> configuration_data { CFDataCreate(kCFAllocatorDefault, configuration_record.data(), configuration_record.size()) };
    RetainedRef<CFMutableDictionaryRef> atoms { CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) };
    CFDictionarySetValue(atoms.ref(), CFSTR("vpcC"), configuration_data.ref());

    RetainedRef<CFMutableDictionaryRef> extensions { CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) };
    CFDictionarySetValue(extensions.ref(), kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, atoms.ref());

    CMVideoFormatDescriptionRef description = nullptr;
    if (CMVideoFormatDescriptionCreate(kCFAllocatorDefault, codec_type, size.width(), size.height(), extensions.ref(), &description) != noErr)
        return {};

    RetainedRef<CFMutableDictionaryRef> specification { CFDictionaryCreateMutable(kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) };
    CFDictionarySetValue(specification.ref(), kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder, kCFBooleanTrue);
    CFDictionarySetValue(specification.ref(), kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, atoms.ref());

    return DecoderFormat { RetainedRef<CMVideoFormatDescriptionRef> { description }, move(specification), destination_attributes_for_bit_depth(bit_depth), color_parameters.cicp };
}

Optional<DecoderFormat> av1_decoder_format(CMVideoCodecType codec_type, Codecs::AV1::Parameters const& parameters, Gfx::IntSize size)
{
    auto configuration_record = av1_configuration_record(parameters);
    RetainedRef<CFDataRef> configuration_data { CFDataCreate(kCFAllocatorDefault, configuration_record.data(), configuration_record.size()) };
    RetainedRef<CFMutableDictionaryRef> atoms { CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) };
    CFDictionarySetValue(atoms.ref(), CFSTR("av1C"), configuration_data.ref());

    RetainedRef<CFMutableDictionaryRef> extensions { CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) };
    CFDictionarySetValue(extensions.ref(), kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, atoms.ref());

    CMVideoFormatDescriptionRef description = nullptr;
    if (CMVideoFormatDescriptionCreate(kCFAllocatorDefault, codec_type, size.width(), size.height(), extensions.ref(), &description) != noErr)
        return {};

    RetainedRef<CFMutableDictionaryRef> specification { CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) };
    CFDictionarySetValue(specification.ref(), kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder, kCFBooleanTrue);

    return DecoderFormat { RetainedRef<CMVideoFormatDescriptionRef> { description }, move(specification), destination_attributes_for_bit_depth(parameters.bit_depth), parameters.optional_fields.cicp };
}

Optional<DecoderFormat> decoder_format_for_frame(CodecID codec_id, CodedFrame const& coded_frame)
{
    auto codec_type = codec_type_from_codec_id(codec_id);
    if (codec_type == 0)
        return {};

    switch (codec_id) {
    case CodecID::VP9: {
        auto header = Codecs::VP9::parse_frame_header(coded_frame.data());
        if (!header.has_value())
            return {};
        return vp9_decoder_format(codec_type, header->profile, header->bit_depth, header->color_parameters, header->size);
    }
    case CodecID::AV1: {
        auto sequence_header = Codecs::AV1::parse_sequence_header(coded_frame.data());
        if (!sequence_header.has_value())
            return {};
        return av1_decoder_format(codec_type, sequence_header->parameters, sequence_header->max_frame_size);
    }
    default:
        return {};
    }
}

ParsedCodec normalize_parsed_codec_for_support_keying(ParsedCodec const& codec)
{
    switch (codec.codec_id()) {
    case CodecID::VP9: {
        auto parameters = codec.vp9_parameters().value_or({ .profile = 0, .level = 0, .bit_depth = 8, .color_parameters = {} });
        parameters.level = 0;
        parameters.color_parameters.cicp = {};
        return ParsedCodec { parameters };
    }
    case CodecID::AV1: {
        auto parameters = codec.av1_parameters().value_or({ .profile = 0, .level = 0, .tier = Codecs::AV1::Tier::Main, .bit_depth = 8, .optional_fields = {} });
        parameters.level = 0;
        parameters.tier = Codecs::AV1::Tier::Main;
        parameters.optional_fields.cicp = {};
        return ParsedCodec { parameters };
    }
    case CodecID::H264: {
        auto parameters = codec.h264_parameters();
        if (!parameters.has_value())
            return codec;
        auto profile = parameters->profile();
        if (!profile.has_value())
            return codec;
        return ParsedCodec { Codecs::H264::canonical_parameters_for_profile(*profile) };
    }
    default:
        return codec;
    }
}

}

struct H264State {
    Codecs::H264::ParameterSetStore parameter_sets;
    u8 nal_unit_length_size { 4 };
    bool dirty { true };
    bool access_unit_is_non_reference { false };

    DecoderErrorOr<void> apply_nal_unit(ReadonlyBytes nal_unit)
    {
        auto result = parameter_sets.apply_nal_unit(nal_unit);
        if (result.is_error()) {
            auto error = result.release_error();
            if (error.is_errno() && error.code() == ENOMEM)
                return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to store H.264 parameter set"sv);
            return DecoderError::corrupted(error.string_literal());
        }
        dirty |= result.release_value();
        return {};
    }

    DecoderErrorOr<void> apply_configuration(ReadonlyBytes configuration)
    {
        if (configuration.is_empty())
            return {};
        auto sets = Codecs::H264::parse_parameter_sets_from_configuration_record(configuration);
        if (!sets.has_value())
            return DecoderError::corrupted("Invalid H.264 configuration record"sv);

        parameter_sets = {};
        nal_unit_length_size = sets->nal_unit_length_size;
        dirty = true;

        for (auto nal_unit : sets->sequence)
            TRY(apply_nal_unit(nal_unit));
        for (auto nal_unit : sets->picture)
            TRY(apply_nal_unit(nal_unit));
        return {};
    }

    Optional<DecoderFormat> decoder_format() const
    {
        // All retained definitions fit inline, so assembling the platform's arrays never allocates.
        Vector<u8 const*, 288> set_pointers;
        Vector<size_t, 288> set_sizes;
        u8 reorder_frame_count = 0;
        for (auto const& set : parameter_sets.sequence_parameter_sets()) {
            set_pointers.append(set.nal_unit.data());
            set_sizes.append(set.nal_unit.size());
            // Bound every SPS a slice could select, without parsing slice headers to track activation.
            reorder_frame_count = max(reorder_frame_count, set.parameters.max_num_reorder_frames);
        }
        if (set_pointers.is_empty())
            return {};
        auto sequence_count = set_pointers.size();
        for (auto const& set : parameter_sets.picture_parameter_sets()) {
            // A PPS may arrive before the SPS it references. Retain it until that SPS is available.
            if (!parameter_sets.sequence_parameter_set(set.parameters.seq_parameter_set_id))
                continue;
            set_pointers.append(set.nal_unit.data());
            set_sizes.append(set.nal_unit.size());
        }
        if (set_pointers.size() == sequence_count)
            return {};

        CMVideoFormatDescriptionRef description = nullptr;
        if (CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, set_pointers.size(), set_pointers.data(), set_sizes.data(), nal_unit_length_size, &description) != noErr)
            return {};

        RetainedRef<CFMutableDictionaryRef> specification { CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) };
        CFDictionarySetValue(specification.ref(), kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder, kCFBooleanTrue);
        return DecoderFormat { RetainedRef<CMVideoFormatDescriptionRef> { description }, move(specification), destination_attributes_for_bit_depth(8), CodingIndependentCodePoints {}, reorder_frame_count };
    }
};

static Optional<DecoderFormat> decoder_format_for_parsed_codec(ParsedCodec const& codec)
{
    static constexpr Gfx::IntSize GENERALLY_SUPPORTED_SIZE { 640, 480 };

    auto codec_type = codec_type_from_codec_id(codec.codec_id());
    if (codec_type == 0)
        return {};

    switch (codec.codec_id()) {
    case CodecID::VP9: {
        auto parameters = codec.vp9_parameters();
        if (!parameters.has_value())
            return {};
        return vp9_decoder_format(codec_type, parameters->profile, parameters->bit_depth, parameters->color_parameters, GENERALLY_SUPPORTED_SIZE);
    }
    case CodecID::AV1: {
        auto parameters = codec.av1_parameters();
        if (!parameters.has_value())
            return {};
        return av1_decoder_format(codec_type, *parameters, GENERALLY_SUPPORTED_SIZE);
    }
    case CodecID::H264: {
        auto parameters = codec.h264_parameters();
        if (!parameters.has_value())
            return {};
        auto profile = parameters->profile();
        if (!profile.has_value())
            return {};
        auto record = Codecs::H264::representative_configuration_record_for_profile(*profile);
        H264State state;
        if (state.apply_configuration(record).is_error())
            return {};
        return state.decoder_format();
    }
    default:
        return {};
    }
}

static bool hardware_can_decode(ParsedCodec const& codec)
{
    auto codec_type = codec_type_from_codec_id(codec.codec_id());
    VTRegisterSupplementalVideoDecoderIfAvailable(codec_type);

    auto format = decoder_format_for_parsed_codec(codec);
    if (!format.has_value())
        return false;

    VTDecompressionSessionRef session = nullptr;
    if (VTDecompressionSessionCreate(kCFAllocatorDefault, format->description.ref(), format->specification.ref(), format->destination_attributes.ref(), nullptr, &session) != noErr)
        return false;

    VTDecompressionSessionInvalidate(session);
    CFRelease(session);
    return true;
}

Optional<DecoderCapabilities> VideoToolboxVideoDecoder::capabilities(ParsedCodec const& codec)
{
    if (codec_type_from_codec_id(codec.codec_id()) == 0)
        return {};

    auto support_key = normalize_parsed_codec_for_support_keying(codec);

    // Building a session to answer this costs enough that the answer is worth keeping.
    static NeverDestroyed<Sync::Mutex> support_mutex;
    static NeverDestroyed<HashMap<ParsedCodec, bool>> support_by_format;

    Sync::MutexLocker locker { *support_mutex };
    auto cached_support = support_by_format->get(support_key);
    auto supported = cached_support.has_value() ? *cached_support : hardware_can_decode(support_key);
    if (!cached_support.has_value())
        support_by_format->set(support_key, supported);

    if (!supported)
        return {};
    return DecoderCapabilities { .smooth = true, .power_efficient = true };
}

DecoderErrorOr<NonnullOwnPtr<VideoToolboxVideoDecoder>> VideoToolboxVideoDecoder::try_create(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    if (codec_type_from_codec_id(codec_id) == 0)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "VideoToolbox has no decoder for codec {}", codec_id);

    auto surface_pool_result = VideoFrameSurfacePool::create();
    if (surface_pool_result.is_error())
        return DecoderError::format(DecoderErrorCategory::Memory, "Failed to create a video surface pool: {}", surface_pool_result.release_error());

    auto decoder = DECODER_TRY_ALLOC(adopt_nonnull_own_or_enomem(new (nothrow) VideoToolboxVideoDecoder(codec_id, surface_pool_result.release_value())));
    if (codec_id == CodecID::H264) {
        decoder->m_h264_state = DECODER_TRY_ALLOC(adopt_nonnull_own_or_enomem(new (nothrow) H264State));
        TRY(decoder->m_h264_state->apply_configuration(codec_initialization_data));
    }
    return decoder;
}

VideoToolboxVideoDecoder::VideoToolboxVideoDecoder(CodecID codec_id, NonnullRefPtr<VideoFrameSurfacePool> surface_pool)
    : m_codec_id(codec_id)
    , m_surface_pool(move(surface_pool))
{
}

VideoToolboxVideoDecoder::~VideoToolboxVideoDecoder()
{
    // Tearing down the session hands the media engine's remaining frames to the output callback, which touches
    // members that would otherwise already be gone by the time the session is destroyed.
    m_session.clear();
}

DecoderErrorOr<void> VideoToolboxVideoDecoder::ensure_session_for_frame(CodedFrame const& coded_frame)
{
    Optional<DecoderFormat> format;
    if (m_h264_state) {
        if (auto configuration = coded_frame.new_codec_configuration(); configuration.has_value())
            TRY(m_h264_state->apply_configuration(*configuration));

        auto saw_coded_slice = false;
        auto every_coded_slice_is_non_reference = true;
        Codecs::NALUnitIterator iterator { coded_frame.data(), m_h264_state->nal_unit_length_size };
        for (auto nal_unit = iterator.next(); nal_unit.has_value(); nal_unit = iterator.next()) {
            TRY(m_h264_state->apply_nal_unit(*nal_unit));
            if (!Codecs::H264::is_coded_slice((*nal_unit)[0]))
                continue;
            saw_coded_slice = true;
            if (Codecs::H264::nal_ref_idc((*nal_unit)[0]) != 0)
                every_coded_slice_is_non_reference = false;
        }
        if (iterator.has_error())
            return DecoderError::corrupted("Invalid H.264 NAL unit length"sv);
        m_h264_state->access_unit_is_non_reference = saw_coded_slice && every_coded_slice_is_non_reference;

        if (!m_h264_state->dirty && m_session)
            return {};
        format = m_h264_state->decoder_format();
        if (!format.has_value())
            return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "H.264 parameter sets do not yet describe a usable format"sv);
    } else {
        format = decoder_format_for_frame(m_codec_id, coded_frame);
    }

    if (m_session != nullptr) {
        if (!format.has_value())
            return {};
        if (CMFormatDescriptionEqual(format->description.ref(), m_session->format_description)) {
            if (m_h264_state)
                m_h264_state->dirty = false;
            Sync::MutexLocker locker { m_output_mutex };
            m_reorder_frame_count = format->reorder_frame_count;
            return {};
        }
    }

    if (!format.has_value())
        return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "The stream has not described its format yet"sv);

    VTRegisterSupplementalVideoDecoderIfAvailable(codec_type_from_codec_id(m_codec_id));

    // Session teardown is deferred until after session creation, since we wait for in-flight frames in its destructor.
    // This allows the in-flight frames to be processed while the new session is being initialized.
    auto old_session = move(m_session);

    auto session = make<Session>(*this);
    session->cicp = format->cicp;
    session->format_description = static_cast<CMVideoFormatDescriptionRef>(CFRetain(format->description.ref()));

    VTDecompressionOutputCallbackRecord callback {
        [](void* session_reference, void* frame_reference, OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image_buffer, CMTime presentation_timestamp, CMTime presentation_duration) {
            auto& session = *static_cast<Session*>(session_reference);
            auto timestamp = duration_from_cm_time(presentation_timestamp);
            auto duration = duration_from_cm_time(presentation_duration);
            session.decoder.note_decode_completed(session.cicp, static_cast<u64>(reinterpret_cast<uintptr_t>(frame_reference)), status, image_buffer, timestamp, duration);
        },
        session.ptr()
    };
    if (VTDecompressionSessionCreate(kCFAllocatorDefault, session->format_description, format->specification.ref(), format->destination_attributes.ref(), &callback, &session->session) != noErr)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "VideoToolbox has no hardware decoder for this format"sv);

    {
        Sync::MutexLocker locker { m_output_mutex };
        m_reorder_frame_count = format->reorder_frame_count;
    }
    if (m_h264_state)
        m_h264_state->dirty = false;
    m_session = move(session);
    return {};
}

void VideoToolboxVideoDecoder::note_decode_completed(CodingIndependentCodePoints const& cicp, u64 generation, i32 status, void* image_buffer, AK::Duration timestamp, AK::Duration duration)
{
    Sync::MutexLocker locker { m_output_mutex };

    if (generation == m_generation.load(AK::MemoryOrder::memory_order_relaxed)) {
        if (status != noErr)
            note_decode_failure_while_locked(status);
        else if (image_buffer != nullptr)
            enqueue_decoded_output_while_locked(cicp, image_buffer, timestamp, duration);
    }

    note_frame_left_the_media_engine_while_locked();
}

void VideoToolboxVideoDecoder::note_frame_left_the_media_engine_while_locked()
{
    auto previous = m_frames_in_flight.fetch_sub(1, AK::MemoryOrder::memory_order_relaxed);
    VERIFY(previous > 0);
    m_output_arrived.broadcast();
}

void VideoToolboxVideoDecoder::note_decode_failure_while_locked(i32 status)
{
    if (m_decode_failure.has_value())
        return;
    m_decode_failure = DecoderError::format(DecoderErrorCategory::Corrupted, "VideoToolbox failed to decode a frame with status {}", status);
}

void VideoToolboxVideoDecoder::insert_output_in_presentation_order_while_locked(DecodedOutput&& output)
{
    auto insertion_index = m_outputs.size();
    while (insertion_index > 0 && m_outputs[insertion_index - 1].timestamp > output.timestamp)
        insertion_index--;
    m_outputs.insert(insertion_index, move(output));
}

void VideoToolboxVideoDecoder::enqueue_decoded_output_while_locked(CodingIndependentCodePoints const& cicp, void* image_buffer, AK::Duration timestamp, AK::Duration duration)
{
    auto* pixel_buffer = static_cast<CVPixelBufferRef>(image_buffer);
    auto* io_surface = CVPixelBufferGetIOSurface(pixel_buffer);
    if (io_surface == nullptr)
        return;

    auto surface_or_error = VideoSurface::create(Core::IOSurfaceHandle::from_ref(io_surface));
    if (surface_or_error.is_error())
        return;

    auto pixel_format = CVPixelBufferGetPixelFormatType(pixel_buffer);
    auto is_ten_bit = pixel_format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange || pixel_format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;

    if (m_decode_failure.has_value())
        return;

    DecodedOutput output {
        .surface = surface_or_error.release_value(),
        .timestamp = timestamp,
        .duration = duration,
        .size = { static_cast<int>(CVPixelBufferGetWidth(pixel_buffer)), static_cast<int>(CVPixelBufferGetHeight(pixel_buffer)) },
        .bit_depth = static_cast<u8>(is_ten_bit ? 10 : 8),
        .subsampling = Subsampling::yuv420(),
        .cicp = cicp,
    };
    insert_output_in_presentation_order_while_locked(move(output));
}

DecoderErrorOr<void> VideoToolboxVideoDecoder::receive_coded_data(CodedFrame const& coded_frame, DecodeIntent intent)
{
    if (m_reached_end_of_stream)
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "VideoToolbox has been drained"sv);

    TRY(ensure_session_for_frame(coded_frame));

    if (intent == DecodeIntent::Reference) {
        if (m_h264_state && m_h264_state->access_unit_is_non_reference)
            return {};
    }

    auto data = coded_frame.data();

    auto block_was_released = false;
    CMBlockBufferCustomBlockSource block_source {};
    block_source.version = kCMBlockBufferCustomBlockSourceVersion;
    block_source.AllocateBlock = nullptr;
    block_source.FreeBlock = [](void* reference, void*, size_t) { *static_cast<bool*>(reference) = true; };
    block_source.refCon = &block_was_released;

    OSStatus status = noErr;
    {
        RetainedRef<CMBlockBufferRef> block_buffer;
        if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, const_cast<u8*>(data.data()), data.size(), nullptr, &block_source, 0, data.size(), 0, block_buffer.out()) != noErr)
            return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate a VideoToolbox block buffer"sv);

        CMSampleTimingInfo timing {
            .duration = cm_time_from_duration(coded_frame.duration()),
            .presentationTimeStamp = cm_time_from_duration(coded_frame.presentation_timestamp()),
            .decodeTimeStamp = cm_time_from_duration(coded_frame.decode_timestamp()),
        };
        size_t sample_size = data.size();
        RetainedRef<CMSampleBufferRef> sample_buffer;
        if (CMSampleBufferCreate(kCFAllocatorDefault, block_buffer.ref(), true, nullptr, nullptr, m_session->format_description, 1, 1, &timing, 1, &sample_size, sample_buffer.out()) != noErr)
            return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to assemble a VideoToolbox sample buffer"sv);

        VTDecodeFrameFlags flags = kVTDecodeFrame_EnableAsynchronousDecompression;
        if (intent == DecodeIntent::Reference)
            flags |= kVTDecodeFrame_DoNotOutputFrame;

        if (m_frames_in_flight.load(AK::MemoryOrder::memory_order_relaxed) >= MAXIMUM_FRAMES_IN_FLIGHT)
            return DecoderError::with_description(DecoderErrorCategory::TryAgain, "VideoToolbox is holding as many frames as it may"sv);
        m_frames_in_flight.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
        auto generation = m_generation.load(AK::MemoryOrder::memory_order_relaxed);

        VTDecodeInfoFlags info_flags = 0;
        auto* frame_reference = reinterpret_cast<void*>(static_cast<uintptr_t>(generation));
        status = VTDecompressionSessionDecodeFrame(m_session->session, sample_buffer.ref(), flags, frame_reference, &info_flags);
    }

    VERIFY(block_was_released);

    if (status != noErr) {
        Sync::MutexLocker locker { m_output_mutex };
        note_frame_left_the_media_engine_while_locked();
        return DecoderError::format(DecoderErrorCategory::Corrupted, "VideoToolbox rejected a frame with status {}", status);
    }
    return {};
}

void VideoToolboxVideoDecoder::signal_end_of_stream()
{
    if (m_session != nullptr)
        VTDecompressionSessionWaitForAsynchronousFrames(m_session->session);
    m_reached_end_of_stream = true;
}

bool VideoToolboxVideoDecoder::may_pull_frame_from_reorder_queue_while_locked() const
{
    auto frames_that_may_still_be_preceded = m_reorder_frame_count;
    if (m_reached_end_of_stream)
        frames_that_may_still_be_preceded = 0;
    return m_outputs.size() > frames_that_may_still_be_preceded;
}

DecoderErrorOr<NonnullRefPtr<VideoFrame>> VideoToolboxVideoDecoder::take_next_output(CodingIndependentCodePoints const& container_cicp)
{
    Sync::MutexLocker locker { m_output_mutex };
    while (true) {
        if (may_pull_frame_from_reorder_queue_while_locked())
            break;

        if (m_decode_failure.has_value())
            return m_decode_failure.release_value();
        if (m_reached_end_of_stream)
            return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "VideoToolbox has been drained"sv);
        if (m_frames_in_flight < MAXIMUM_FRAMES_IN_FLIGHT)
            return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "VideoToolbox has no frame that later ones cannot precede"sv);

        m_output_arrived.wait();
    }

    auto const& output = m_outputs.first();

    auto acquired_slot = m_surface_pool->try_acquire(output.surface);
    if (!acquired_slot.has_value())
        return DecoderError::with_description(DecoderErrorCategory::TryAgain, "Every video frame slot is held elsewhere"sv);

    auto pool_slot_result = m_surface_pool->try_adopt_acquired_slot(*acquired_slot);
    if (pool_slot_result.is_error())
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate a pooled frame slot reference"sv);

    auto cicp = container_cicp;
    cicp.adopt_specified_values(output.cicp);

    auto frame = DECODER_TRY_ALLOC(try_make_ref_counted<VideoFrame>(
        output.timestamp, output.duration, output.size.to_type<u32>(),
        output.bit_depth, output.subsampling, cicp, pool_slot_result.release_value()));
    m_outputs.remove(0);
    return frame;
}

void VideoToolboxVideoDecoder::flush()
{
    m_reached_end_of_stream = false;

    Sync::MutexLocker locker { m_output_mutex };
    m_generation.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
    m_outputs.clear();
    m_decode_failure.clear();
}

}
