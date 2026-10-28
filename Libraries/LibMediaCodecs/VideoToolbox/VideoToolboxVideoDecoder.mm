/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibMediaCodecs/Codecs/AV1.h>
#include <LibMediaCodecs/Codecs/H264.h>
#include <LibMediaCodecs/Codecs/H265.h>
#include <LibMediaCodecs/Codecs/NALUnit.h>
#include <LibMediaCodecs/Codecs/VP9.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoToolbox/VideoToolboxVideoDecoder.h>

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dispatch/dispatch.h>
#include <sys/sysctl.h>

namespace Media::VideoToolbox {

struct VideoToolboxVideoDecoder::Session {
    AK_ALLOC_WITH_KMALLOC;

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

// The media engine recycles a pixel buffer once nothing retains it, so retaining it is what keeps its surface's
// pixels in place while they are shown.
class PixelBufferContents final : public VideoSurfaceContents {
public:
    explicit PixelBufferContents(CVPixelBufferRef pixel_buffer)
        : m_pixel_buffer(CVPixelBufferRetain(pixel_buffer))
    {
    }

private:
    RetainedRef<CVPixelBufferRef> m_pixel_buffer;
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
    case CodecID::H265:
        return kCMVideoCodecType_HEVC;
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

    RetainedRef<CFMutableDictionaryRef> attributes { CFDictionaryCreateMutable(kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) };
    CFDictionarySetValue(attributes.ref(), kCVPixelBufferPixelFormatTypeKey, pixel_format_numbers.ref());

    RetainedRef<CFMutableDictionaryRef> io_surface_properties { CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) };
    CFDictionarySetValue(attributes.ref(), kCVPixelBufferIOSurfacePropertiesKey, io_surface_properties.ref());
    return attributes;
}

bool hardware_decoding_is_required(CodecID codec_id)
{
    switch (codec_id) {
    case CodecID::H264:
    case CodecID::H265:
        return false;
    default:
        return true;
    }
}

bool is_running_in_virtual_machine()
{
    static bool const running_in_virtual_machine = [] {
        int hypervisor_present = 0;
        size_t size = sizeof(hypervisor_present);
        if (sysctlbyname("kern.hv_vmm_present", &hypervisor_present, &size, nullptr, 0) != 0)
            return false;
        return hypervisor_present != 0;
    }();
    return running_in_virtual_machine;
}

DecoderErrorOr<VTDecompressionSessionRef> create_decompression_session(CodecID codec_id, DecoderFormat const& format, VTDecompressionOutputCallbackRecord const* callback)
{
    // A virtual machine's hardware decoding goes through paravirtualization, which leaks a connection per process.
    auto hardware_is_usable = !is_running_in_virtual_machine();
    if (hardware_decoding_is_required(codec_id) && !hardware_is_usable)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "VideoToolbox has no decoder for this format"sv);

    VTRegisterSupplementalVideoDecoderIfAvailable(codec_type_from_codec_id(codec_id));

    RetainedRef<CFMutableDictionaryRef> specification { CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) };
    if (hardware_decoding_is_required(codec_id))
        CFDictionarySetValue(specification.ref(), kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder, kCFBooleanTrue);
    else
        CFDictionarySetValue(specification.ref(), kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder, hardware_is_usable ? kCFBooleanTrue : kCFBooleanFalse);

    VTDecompressionSessionRef session = nullptr;
    if (VTDecompressionSessionCreate(kCFAllocatorDefault, format.description.ref(), specification.ref(), format.destination_attributes.ref(), callback, &session) != noErr)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "VideoToolbox has no decoder for this format"sv);
    return session;
}

bool session_uses_hardware_decoder(VTDecompressionSessionRef session)
{
    RetainedRef<CFBooleanRef> uses_hardware;
    if (VTSessionCopyProperty(session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, kCFAllocatorDefault, uses_hardware.out()) != noErr)
        return false;
    return uses_hardware.ref() != nullptr && CFBooleanGetValue(uses_hardware.ref());
}

}

struct FormatState {
    virtual ~FormatState() = default;

    // Takes whatever the frame says about the stream's format.
    virtual DecoderErrorOr<void> apply_coded_frame(CodedFrame const&) = 0;
    virtual Optional<DecoderFormat> decoder_format() const = 0;

    // Set while the tracked format may differ from the one the current session was built for.
    bool dirty { true };
    // Whether the last frame's picture is a random access point, which the pictures following it are associated with.
    bool access_unit_is_random_access_point { false };
    // Whether that random access point's leading pictures may reference pictures that precede it.
    bool access_unit_begins_open_gop { false };
    // Whether the last frame's picture is such a leading picture.
    bool access_unit_is_open_gop_leading_picture { false };
};

struct H264State final : FormatState {
    AK_ALLOC_WITH_KMALLOC;

    Codecs::H264::ParameterSetStore parameter_sets;
    u8 nal_unit_length_size { 4 };

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

    virtual DecoderErrorOr<void> apply_coded_frame(CodedFrame const& coded_frame) override
    {
        if (auto configuration = coded_frame.new_codec_configuration(); configuration.has_value())
            TRY(apply_configuration(*configuration));

        Codecs::NALUnitIterator iterator { coded_frame.data(), nal_unit_length_size };
        for (auto nal_unit = iterator.next(); nal_unit.has_value(); nal_unit = iterator.next())
            TRY(apply_nal_unit(*nal_unit));
        if (iterator.has_error())
            return DecoderError::corrupted("Invalid H.264 NAL unit length"sv);
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

    virtual Optional<DecoderFormat> decoder_format() const override
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

        return DecoderFormat { RetainedRef<CMVideoFormatDescriptionRef> { description }, destination_attributes_for_bit_depth(8), CodingIndependentCodePoints {}, reorder_frame_count };
    }
};

struct H265State final : FormatState {
    AK_ALLOC_WITH_KMALLOC;

    Codecs::H265::ParameterSetStore parameter_sets;
    u8 nal_unit_length_size { 4 };

    DecoderErrorOr<void> apply_nal_unit(ReadonlyBytes nal_unit)
    {
        auto result = parameter_sets.apply_nal_unit(nal_unit);
        if (result.is_error()) {
            auto error = result.release_error();
            if (error.is_errno() && error.code() == ENOMEM)
                return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to store H.265 parameter set"sv);
            return DecoderError::corrupted(error.string_literal());
        }
        dirty |= result.release_value();
        return {};
    }

    virtual DecoderErrorOr<void> apply_coded_frame(CodedFrame const& coded_frame) override
    {
        if (auto configuration = coded_frame.new_codec_configuration(); configuration.has_value())
            TRY(apply_configuration(*configuration));

        auto saw_random_access_point = false;
        auto saw_open_gop_start = false;
        auto saw_open_gop_leading_picture = false;
        Codecs::NALUnitIterator iterator { coded_frame.data(), nal_unit_length_size };
        for (auto nal_unit = iterator.next(); nal_unit.has_value(); nal_unit = iterator.next()) {
            TRY(apply_nal_unit(*nal_unit));
            auto header = Codecs::H265::parse_nal_unit_header(*nal_unit);
            if (!header.has_value() || !Codecs::H265::is_coded_slice(*header))
                continue;
            if (Codecs::H265::is_random_access_point(*header))
                saw_random_access_point = true;
            if (Codecs::H265::has_random_access_skipped_leading_pictures(*header))
                saw_open_gop_start = true;
            if (Codecs::H265::is_random_access_skipped_leading(*header))
                saw_open_gop_leading_picture = true;
        }
        if (iterator.has_error())
            return DecoderError::corrupted("Invalid H.265 NAL unit length"sv);
        access_unit_is_random_access_point = saw_random_access_point;
        access_unit_begins_open_gop = saw_open_gop_start;
        access_unit_is_open_gop_leading_picture = saw_open_gop_leading_picture;
        return {};
    }

    DecoderErrorOr<void> apply_configuration(ReadonlyBytes configuration)
    {
        if (configuration.is_empty())
            return {};
        auto sets = Codecs::H265::parse_parameter_sets_from_configuration_record(configuration);
        if (!sets.has_value())
            return DecoderError::corrupted("Invalid H.265 configuration record"sv);

        parameter_sets = {};
        nal_unit_length_size = sets->nal_unit_length_size;
        dirty = true;

        for (auto nal_unit : sets->video)
            TRY(apply_nal_unit(nal_unit));
        for (auto nal_unit : sets->sequence)
            TRY(apply_nal_unit(nal_unit));
        for (auto nal_unit : sets->picture)
            TRY(apply_nal_unit(nal_unit));
        return {};
    }

    virtual Optional<DecoderFormat> decoder_format() const override
    {
        // All retained definitions fit inline, so assembling the platform's arrays never allocates.
        Vector<u8 const*, 96> set_pointers;
        Vector<size_t, 96> set_sizes;
        auto append = [&](auto const& set) {
            set_pointers.append(set.nal_unit.data());
            set_sizes.append(set.nal_unit.size());
        };

        // A set is only usable once everything it names has arrived. The platform requires at least one of each type,
        // and takes them in this order.
        u8 reorder_frame_count = 0;
        u8 bit_depth = 8;
        for (auto const& set : parameter_sets.video_parameter_sets()) {
            auto is_referenced = false;
            for (auto const& sequence_set : parameter_sets.sequence_parameter_sets())
                is_referenced |= sequence_set.parameters.sps_video_parameter_set_id == set.parameters.vps_video_parameter_set_id;
            if (is_referenced)
                append(set);
        }
        if (set_pointers.is_empty())
            return {};

        auto video_count = set_pointers.size();
        for (auto const& set : parameter_sets.sequence_parameter_sets()) {
            if (!parameter_sets.video_parameter_set(set.parameters.sps_video_parameter_set_id))
                continue;
            append(set);
            // Bound every sequence set a slice could select, without parsing slice headers to track activation.
            reorder_frame_count = max(reorder_frame_count, set.parameters.sps_max_num_reorder_pics);
            bit_depth = max(bit_depth, set.parameters.bit_depth_luma);
        }
        if (set_pointers.size() == video_count)
            return {};

        auto sequence_count = set_pointers.size();
        for (auto const& set : parameter_sets.picture_parameter_sets()) {
            // A picture set may arrive before the sequence set it names, which in turn needs its own video set.
            auto const* sequence_set = parameter_sets.sequence_parameter_set(set.parameters.pps_seq_parameter_set_id);
            if (sequence_set == nullptr || !parameter_sets.video_parameter_set(sequence_set->parameters.sps_video_parameter_set_id))
                continue;
            append(set);
        }
        if (set_pointers.size() == sequence_count)
            return {};

        CMVideoFormatDescriptionRef description = nullptr;
        if (CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault, set_pointers.size(), set_pointers.data(), set_sizes.data(), nal_unit_length_size, nullptr, &description) != noErr)
            return {};

        return DecoderFormat { RetainedRef<CMVideoFormatDescriptionRef> { description }, destination_attributes_for_bit_depth(bit_depth), CodingIndependentCodePoints {}, reorder_frame_count };
    }
};

struct VP9State final : FormatState {
    AK_ALLOC_WITH_KMALLOC;

    Optional<Codecs::VP9::FrameHeader> header;

    virtual DecoderErrorOr<void> apply_coded_frame(CodedFrame const& coded_frame) override
    {
        auto frame_header = Codecs::VP9::parse_frame_header(coded_frame.data());
        if (frame_header.has_value() && frame_header != header) {
            header = frame_header;
            dirty = true;
        }
        return {};
    }

    virtual Optional<DecoderFormat> decoder_format() const override
    {
        if (!header.has_value())
            return {};

        auto configuration_record = vp9_configuration_record(header->profile, header->bit_depth, header->color_parameters);
        RetainedRef<CFDataRef> configuration_data { CFDataCreate(kCFAllocatorDefault, configuration_record.data(), configuration_record.size()) };
        RetainedRef<CFMutableDictionaryRef> atoms { CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) };
        CFDictionarySetValue(atoms.ref(), CFSTR("vpcC"), configuration_data.ref());

        RetainedRef<CFMutableDictionaryRef> extensions { CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) };
        CFDictionarySetValue(extensions.ref(), kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, atoms.ref());

        CMVideoFormatDescriptionRef description = nullptr;
        if (CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_VP9, header->size.width(), header->size.height(), extensions.ref(), &description) != noErr)
            return {};

        return DecoderFormat { RetainedRef<CMVideoFormatDescriptionRef> { description }, destination_attributes_for_bit_depth(header->bit_depth), header->color_parameters.cicp };
    }
};

struct AV1State final : FormatState {
    AK_ALLOC_WITH_KMALLOC;

    Optional<Codecs::AV1::SequenceHeader> sequence_header;

    virtual DecoderErrorOr<void> apply_coded_frame(CodedFrame const& coded_frame) override
    {
        auto frame_sequence_header = Codecs::AV1::parse_sequence_header(coded_frame.data());
        if (frame_sequence_header.has_value() && frame_sequence_header != sequence_header) {
            sequence_header = frame_sequence_header;
            dirty = true;
        }
        return {};
    }

    virtual Optional<DecoderFormat> decoder_format() const override
    {
        if (!sequence_header.has_value())
            return {};

        auto const& parameters = sequence_header->parameters;
        auto configuration_record = av1_configuration_record(parameters);
        RetainedRef<CFDataRef> configuration_data { CFDataCreate(kCFAllocatorDefault, configuration_record.data(), configuration_record.size()) };
        RetainedRef<CFMutableDictionaryRef> atoms { CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) };
        CFDictionarySetValue(atoms.ref(), CFSTR("av1C"), configuration_data.ref());

        RetainedRef<CFMutableDictionaryRef> extensions { CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) };
        CFDictionarySetValue(extensions.ref(), kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, atoms.ref());

        auto size = sequence_header->max_frame_size;
        CMVideoFormatDescriptionRef description = nullptr;
        if (CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_AV1, size.width(), size.height(), extensions.ref(), &description) != noErr)
            return {};

        return DecoderFormat { RetainedRef<CMVideoFormatDescriptionRef> { description }, destination_attributes_for_bit_depth(parameters.bit_depth), parameters.optional_fields.cicp };
    }
};

static DecoderErrorOr<NonnullOwnPtr<FormatState>> create_format_state(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    switch (codec_id) {
    case CodecID::H264: {
        auto state = DECODER_TRY_ALLOC(adopt_nonnull_own_or_enomem(new (nothrow) H264State));
        TRY(state->apply_configuration(codec_initialization_data));
        return state;
    }
    case CodecID::H265: {
        auto state = DECODER_TRY_ALLOC(adopt_nonnull_own_or_enomem(new (nothrow) H265State));
        TRY(state->apply_configuration(codec_initialization_data));
        return state;
    }
    case CodecID::VP9:
        return DECODER_TRY_ALLOC(adopt_nonnull_own_or_enomem(new (nothrow) VP9State));
    case CodecID::AV1:
        return DECODER_TRY_ALLOC(adopt_nonnull_own_or_enomem(new (nothrow) AV1State));
    default:
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "VideoToolbox has no decoder for codec {}", codec_id);
    }
}

static Optional<DecoderFormat> decoder_format_for_parsed_codec(ParsedCodec const& codec)
{
    static constexpr Gfx::IntSize GENERALLY_SUPPORTED_SIZE { 640, 480 };

    switch (codec.codec_id()) {
    case CodecID::VP9: {
        auto parameters = codec.vp9_parameters();
        if (!parameters.has_value())
            return {};
        VP9State state;
        state.header = Codecs::VP9::FrameHeader { parameters->profile, parameters->bit_depth, GENERALLY_SUPPORTED_SIZE, parameters->color_parameters };
        return state.decoder_format();
    }
    case CodecID::AV1: {
        auto parameters = codec.av1_parameters();
        if (!parameters.has_value())
            return {};
        AV1State state;
        state.sequence_header = Codecs::AV1::SequenceHeader { *parameters, GENERALLY_SUPPORTED_SIZE };
        return state.decoder_format();
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
    case CodecID::H265: {
        auto parameters = codec.h265_parameters();
        if (!parameters.has_value())
            return {};
        auto profile = parameters->profile();
        if (!profile.has_value())
            return {};
        auto record = Codecs::H265::representative_configuration_record_for_profile(*profile);
        H265State state;
        if (state.apply_configuration(record).is_error())
            return {};
        return state.decoder_format();
    }
    default:
        return {};
    }
}

static Optional<DecoderCapabilities> probe_decoder_capabilities(ParsedCodec const& codec)
{
    auto format = decoder_format_for_parsed_codec(codec);
    if (!format.has_value())
        return {};

    auto session_or_error = create_decompression_session(codec.codec_id(), *format, nullptr);
    if (session_or_error.is_error())
        return {};
    auto* session = session_or_error.release_value();

    auto uses_hardware = hardware_decoding_is_required(codec.codec_id()) || session_uses_hardware_decoder(session);
    VTDecompressionSessionInvalidate(session);
    CFRelease(session);
    return DecoderCapabilities { .smooth = true, .power_efficient = uses_hardware };
}

Optional<DecoderCapabilities> VideoToolboxVideoDecoder::capabilities(ParsedCodec const& codec)
{
    if (codec_type_from_codec_id(codec.codec_id()) == 0)
        return {};

    auto support_key = normalize_parsed_codec_for_support_keying(codec);

    // Building a session to answer this costs enough that the answer is worth keeping.
    static NeverDestroyed<Mutex> support_mutex;
    static NeverDestroyed<HashMap<ParsedCodec, Optional<DecoderCapabilities>>> capabilities_by_format;

    MutexLocker locker { *support_mutex };
    auto cached_capabilities = capabilities_by_format->get(support_key);
    if (cached_capabilities.has_value())
        return *cached_capabilities;

    auto capabilities = probe_decoder_capabilities(support_key);
    capabilities_by_format->set(support_key, capabilities);
    return capabilities;
}

DecoderErrorOr<NonnullOwnPtr<VideoToolboxVideoDecoder>> VideoToolboxVideoDecoder::try_create(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    if (codec_type_from_codec_id(codec_id) == 0)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "VideoToolbox has no decoder for codec {}", codec_id);

    auto format_state = TRY(create_format_state(codec_id, codec_initialization_data));
    return DECODER_TRY_ALLOC(adopt_nonnull_own_or_enomem(new (nothrow) VideoToolboxVideoDecoder(codec_id, move(format_state))));
}

VideoToolboxVideoDecoder::VideoToolboxVideoDecoder(CodecID codec_id, NonnullOwnPtr<FormatState> format_state)
    : m_codec_id(codec_id)
    , m_format_state(move(format_state))
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
    TRY(m_format_state->apply_coded_frame(coded_frame));
    if (!m_format_state->dirty && m_session)
        return {};

    auto format = m_format_state->decoder_format();
    if (!format.has_value())
        return DecoderError::corrupted("The stream has not described a usable format"sv);

    if (m_session != nullptr && CMFormatDescriptionEqual(format->description.ref(), m_session->format_description)) {
        m_format_state->dirty = false;
        MutexLocker locker { m_output_mutex };
        m_reorder_frame_count = format->reorder_frame_count;
        return {};
    }

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
    session->session = TRY(create_decompression_session(m_codec_id, *format, &callback));

    {
        MutexLocker locker { m_output_mutex };
        m_reorder_frame_count = format->reorder_frame_count;
    }
    m_format_state->dirty = false;
    m_session = move(session);
    m_awaiting_first_submitted_frame = true;
    return {};
}

void VideoToolboxVideoDecoder::note_decode_completed(CodingIndependentCodePoints const& cicp, u64 generation, i32 status, void* image_buffer, AK::Duration timestamp, AK::Duration duration)
{
    auto caller_needs_waking = false;
    {
        MutexLocker locker { m_output_mutex };

        if (generation == m_generation.load(AK::MemoryOrder::memory_order_relaxed)) {
            if (status != noErr)
                note_decode_failure_while_locked(status);
            else if (image_buffer != nullptr)
                enqueue_decoded_output_while_locked(cicp, image_buffer, timestamp, duration);
            auto frame_was_decoded_for_reference_only = status == noErr && image_buffer == nullptr;
            caller_needs_waking = !frame_was_decoded_for_reference_only && !m_caller_is_waiting_for_output;
        }

        note_frame_left_the_media_engine_while_locked();
    }

    if (caller_needs_waking && m_output_arrived_callback)
        m_output_arrived_callback();
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

    auto contents_or_error = try_make_ref_counted<PixelBufferContents>(pixel_buffer);
    if (contents_or_error.is_error())
        return;

    DecodedOutput output {
        .surface = surface_or_error.release_value(),
        .contents = contents_or_error.release_value(),
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

    if (m_format_state->access_unit_begins_open_gop)
        m_discarding_open_gop_leading_pictures = m_awaiting_first_submitted_frame;
    else if (m_format_state->access_unit_is_random_access_point)
        m_discarding_open_gop_leading_pictures = false;
    if (m_discarding_open_gop_leading_pictures && m_format_state->access_unit_is_open_gop_leading_picture)
        return {};

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

        // The media engine keeps decoding the frames a flush discarded until it hands them back, so a submission
        // that finds it full waits for one to leave, as taking output does.
        {
            MutexLocker locker { m_output_mutex };
            while (m_frames_in_flight.load(AK::MemoryOrder::memory_order_relaxed) >= MAXIMUM_FRAMES_IN_FLIGHT) {
                m_caller_is_waiting_for_output = true;
                m_output_arrived.wait();
                m_caller_is_waiting_for_output = false;
            }
            m_frames_in_flight.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
        }
        auto generation = m_generation.load(AK::MemoryOrder::memory_order_relaxed);

        VTDecodeInfoFlags info_flags = 0;
        auto* frame_reference = reinterpret_cast<void*>(static_cast<uintptr_t>(generation));
        status = VTDecompressionSessionDecodeFrame(m_session->session, sample_buffer.ref(), flags, frame_reference, &info_flags);
    }

    VERIFY(block_was_released);

    if (status != noErr) {
        MutexLocker locker { m_output_mutex };
        note_frame_left_the_media_engine_while_locked();
        return DecoderError::format(DecoderErrorCategory::Corrupted, "VideoToolbox rejected a frame with status {}", status);
    }
    m_awaiting_first_submitted_frame = false;
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

DecoderErrorOr<NonnullRefPtr<VideoFrame>> VideoToolboxVideoDecoder::take_next_output(CodingIndependentCodePoints const& container_cicp, Optional<AK::Duration> target)
{
    MutexLocker locker { m_output_mutex };
    while (true) {
        // The frame covering a target is the one the caller is after, and nothing decoded later can precede it, so
        // it need not wait behind the reorder window.
        if (target.has_value()) {
            for (size_t index = 0; index < m_outputs.size(); index++) {
                auto const& output = m_outputs[index];
                if (*target < output.timestamp || *target >= output.timestamp + output.duration)
                    continue;
                auto frame = TRY(adopt_output_while_locked(container_cicp, index));
                // Everything the covering frame was queued behind precedes the target.
                m_outputs.remove(0, index + 1);
                return frame;
            }
        }

        if (may_pull_frame_from_reorder_queue_while_locked())
            break;

        if (m_decode_failure.has_value())
            return m_decode_failure.release_value();
        if (m_reached_end_of_stream)
            return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "VideoToolbox has been drained"sv);
        if (m_frames_in_flight < MAXIMUM_FRAMES_IN_FLIGHT)
            return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "VideoToolbox has no frame that later ones cannot precede"sv);

        m_caller_is_waiting_for_output = true;
        m_output_arrived.wait();
        m_caller_is_waiting_for_output = false;
    }

    auto frame = TRY(adopt_output_while_locked(container_cicp, 0));
    m_outputs.remove(0);
    return frame;
}

DecoderErrorOr<NonnullRefPtr<VideoFrame>> VideoToolboxVideoDecoder::adopt_output_while_locked(CodingIndependentCodePoints const& container_cicp, size_t index)
{
    auto const& output = m_outputs[index];

    auto storage = m_surface_budget->try_hold(output.surface, output.contents);
    if (!storage)
        return DecoderError::with_description(DecoderErrorCategory::TryAgain, "Every video surface the decoder may hand out is held elsewhere"sv);

    auto cicp = container_cicp;
    cicp.adopt_specified_values(output.cicp);

    return DECODER_TRY_ALLOC(try_make_ref_counted<VideoFrame>(
        output.timestamp, output.duration, output.size.to_type<u32>(),
        output.bit_depth, output.subsampling, cicp, storage.release_nonnull()));
}

void VideoToolboxVideoDecoder::flush()
{
    m_reached_end_of_stream = false;
    m_awaiting_first_submitted_frame = true;

    MutexLocker locker { m_output_mutex };
    m_generation.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
    m_outputs.clear();
    m_decode_failure.clear();
}

}
