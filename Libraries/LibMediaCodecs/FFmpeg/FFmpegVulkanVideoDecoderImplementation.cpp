/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Diagnostics.h>
#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibMediaCodecs/FFmpeg/FFmpegFunctions.h>
#include <LibMediaCodecs/FFmpeg/FFmpegHelpers.h>
#include <LibMediaCodecs/FFmpeg/FFmpegVulkanVideoDecoderBackend.h>
#include <LibMediaCodecs/FFmpeg/FFmpegVulkanVideoDecoderImplementation.h>
#include <LibMediaCodecs/Limits.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibPlatformFeatures/ProcessFeatures.h>
#include <LibThreading/Thread.h>
#include <string.h>

// A build against another FFmpeg release's headers has already included this inside its namespace.
extern "C" {
#include <libavutil/hwcontext_vulkan.h>
}

namespace FFMPEG_ABI_NAMESPACE {

namespace {

constexpr u32 drm_fourcc(char a, char b, char c, char d)
{
    return static_cast<u32>(a) | (static_cast<u32>(b) << 8) | (static_cast<u32>(c) << 16) | (static_cast<u32>(d) << 24);
}

constexpr u32 DRM_FORMAT_NV12 = drm_fourcc('N', 'V', '1', '2');
constexpr u32 DRM_FORMAT_P010 = drm_fourcc('P', '0', '1', '0');

constexpr AK::Duration DEVICE_WORK_STALL_UNTIL_TREATED_AS_FAILED = AK::Duration::from_seconds(5);

char const* decoder_name_for(CodecID codec_id)
{
    switch (codec_id) {
    case CodecID::VP9:
        return "vp9";
    case CodecID::AV1:
        return "av1";
    case CodecID::H264:
        return "h264";
    case CodecID::H265:
        return "hevc";
    default:
        return nullptr;
    }
}

StringView decode_extension_for(CodecID codec_id)
{
    switch (codec_id) {
    case CodecID::VP9:
        return VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME ""sv;
    case CodecID::AV1:
        return VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME ""sv;
    case CodecID::H264:
        return VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME ""sv;
    case CodecID::H265:
        return VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME ""sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

AVCodec const* vulkan_capable_decoder(FFmpegFunctions const& functions, CodecID codec_id)
{
    auto const* codec = functions.avcodec_find_decoder_by_name(decoder_name_for(codec_id));
    if (codec == nullptr)
        return nullptr;
    for (int index = 0;; index++) {
        auto const* config = functions.avcodec_get_hw_config(codec, index);
        if (config == nullptr)
            return nullptr;
        if (config->device_type == AV_HWDEVICE_TYPE_VULKAN && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0)
            return codec;
    }
}

Optional<VkVideoComponentBitDepthFlagBitsKHR> component_bit_depth_for(u8 bit_depth)
{
    switch (bit_depth) {
    case 8:
        return VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    case 10:
        return VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
    case 12:
        return VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR;
    default:
        return {};
    }
}

void lock_queue(AVHWDeviceContext*, u32 queue_family, u32)
{
    VulkanVideoDevice::the()->lock_queue(queue_family);
}

void unlock_queue(AVHWDeviceContext*, u32 queue_family, u32)
{
    VulkanVideoDevice::the()->unlock_queue(queue_family);
}

// The decoders using one FFmpeg share one device context around the process's device, which lives as long as it
// does. A context only works with the FFmpeg that made it, so each FFmpeg gets its own.
DecoderErrorOr<AVBufferRef*> device_context_for(FFmpegFunctions const& functions, VulkanVideoDevice& device)
{
    static NeverDestroyed<Mutex> mutex;
    static NeverDestroyed<HashMap<FFmpegFunctions const*, AVBufferRef*>> device_context_references;

    MutexLocker locker { *mutex };
    if (auto reference = device_context_references->get(&functions); reference.has_value())
        return *reference;

    auto* reference = functions.av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (reference == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Could not allocate an FFmpeg Vulkan device context"sv);
    ArmedScopeGuard free_reference = [&] { functions.av_buffer_unref(&reference); };

    auto& device_context = *reinterpret_cast<AVHWDeviceContext*>(reference->data);
    auto& vulkan_context = *static_cast<AVVulkanDeviceContext*>(device_context.hwctx);
    vulkan_context.get_proc_addr = vkGetInstanceProcAddr;
    vulkan_context.inst = device.instance();
    vulkan_context.phys_dev = device.physical_device();
    vulkan_context.act_dev = device.logical_device();
    vulkan_context.device_features = device.enabled_features();
    vulkan_context.enabled_dev_extensions = device.enabled_extensions().data();
    vulkan_context.nb_enabled_dev_extensions = static_cast<int>(device.enabled_extensions().size());
    // From libavutil 59.34.100, the device's queue families are listed. Before then, each purpose names its family.
    [&](auto& context) {
        if constexpr (requires { context.qf; }) {
            if (functions.avutil_version() >= AV_VERSION_INT(59, 34, 100)) {
                for (auto const& family : device.queue_families()) {
                    auto& queue_family = context.qf[context.nb_qf++];
                    queue_family.idx = static_cast<int>(family.index);
                    queue_family.num = 1;
                    queue_family.flags = static_cast<VkQueueFlagBits>(family.flags);
                }
                return;
            }
        }
        // GCC also warns about the deprecated fields being named in the requirement.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        if constexpr (requires { context.queue_family_decode_index; }) {
            auto family_for = [&](VkQueueFlags purpose) {
                for (auto const& family : device.queue_families()) {
                    if ((family.flags & purpose) != 0)
                        return static_cast<int>(family.index);
                }
                VERIFY_NOT_REACHED();
            };
            context.queue_family_comp_index = static_cast<int>(device.compute_queue_family());
            context.nb_comp_queues = 1;
            context.queue_family_tx_index = family_for(VK_QUEUE_TRANSFER_BIT);
            context.nb_tx_queues = 1;
            context.queue_family_decode_index = family_for(VK_QUEUE_VIDEO_DECODE_BIT_KHR);
            context.nb_decode_queues = 1;
        }
#pragma GCC diagnostic pop
    }(vulkan_context);
    // Each context would otherwise keep its own locks on queues that the others submit to as well.
    AK_IGNORE_DIAGNOSTIC("-Wdeprecated-declarations", vulkan_context.lock_queue = lock_queue;)
    AK_IGNORE_DIAGNOSTIC("-Wdeprecated-declarations", vulkan_context.unlock_queue = unlock_queue;)

    if (functions.av_hwdevice_ctx_init(reference) < 0)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "FFmpeg could not use the Vulkan device"sv);

    DECODER_TRY_ALLOC(device_context_references->try_set(&functions, reference));
    free_reference.disarm();
    return reference;
}

bool wait_for_semaphore(VkDevice device, VkSemaphore semaphore, u64 value)
{
    VkSemaphoreWaitInfo wait_info {};
    wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait_info.semaphoreCount = 1;
    wait_info.pSemaphores = &semaphore;
    wait_info.pValues = &value;
    return vkWaitSemaphores(device, &wait_info, static_cast<u64>(DEVICE_WORK_STALL_UNTIL_TREATED_AS_FAILED.to_nanoseconds())) == VK_SUCCESS;
}

AVHWFramesContext& frames_context_of(AVFrame const& frame)
{
    return *reinterpret_cast<AVHWFramesContext*>(frame.hw_frames_ctx->data);
}

struct SemaphoreValue {
    VkSemaphore semaphore { VK_NULL_HANDLE };
    u64 value { 0 };
};

// Reads the semaphore and the value that the frame's latest work signals, which FFmpeg updates on whichever thread
// submits work using the frame.
SemaphoreValue semaphore_value_of(AVFrame const& frame)
{
    auto& frames_context = frames_context_of(frame);
    auto& vulkan_frames_context = *static_cast<AVVulkanFramesContext*>(frames_context.hwctx);
    auto* vulkan_frame = reinterpret_cast<AVVkFrame*>(frame.data[0]);
    vulkan_frames_context.lock_frame(&frames_context, vulkan_frame);
    SemaphoreValue result { vulkan_frame->sem[0], vulkan_frame->sem_value[0] };
    vulkan_frames_context.unlock_frame(&frames_context, vulkan_frame);
    return result;
}

u8 bit_depth_of(AVHWFramesContext const& frames_context)
{
    return frames_context.sw_format == AV_PIX_FMT_P010 ? 10 : 8;
}

// Keeps a decoded image out of FFmpeg's pool, so that it is not decoded over while its frame is held anywhere.
class VulkanFrameContents final : public VideoSurfaceContents {
public:
    VulkanFrameContents(FFmpegFunctions const& functions, AVFrame* frame)
        : m_functions(functions)
        , m_frame(frame)
    {
    }

    virtual ~VulkanFrameContents() override
    {
        m_functions.av_frame_free(&m_frame);
    }

private:
    FFmpegFunctions const& m_functions;
    AVFrame* m_frame { nullptr };
};

}

Optional<DecoderCapabilities> FFmpegVulkanVideoDecoderImplementation::capabilities(FFmpegFunctions const& functions, ParsedCodec const& codec)
{
    return capabilities(functions, codec, ChecksExportableFormat::Yes);
}

Optional<DecoderCapabilities> FFmpegVulkanVideoDecoderImplementation::capabilities(FFmpegFunctions const& functions, ParsedCodec const& codec, ChecksExportableFormat checks_exportable_format)
{
    auto* device = VulkanVideoDevice::the();
    if (device == nullptr)
        return {};

    auto codec_id = codec.codec_id();
    if (!device->has_enabled_extension(decode_extension_for(codec_id)))
        return {};
    if (vulkan_capable_decoder(functions, codec_id) == nullptr)
        return {};
    // Before FFmpeg 7.1, decoding creates a sampler YCbCr conversion on the device.
    if (functions.avcodec_version() < AV_VERSION_INT(61, 13, 100) && !device->has_sampler_ycbcr_conversion())
        return {};

    VkVideoDecodeVP9ProfileInfoKHR vp9_profile {};
    vp9_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PROFILE_INFO_KHR;
    VkVideoDecodeAV1ProfileInfoKHR av1_profile {};
    av1_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR;
    VkVideoDecodeH264ProfileInfoKHR h264_profile {};
    h264_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
    VkVideoDecodeH265ProfileInfoKHR h265_profile {};
    h265_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
    VkVideoDecodeVP9CapabilitiesKHR vp9_capabilities {};
    vp9_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_CAPABILITIES_KHR;
    VkVideoDecodeAV1CapabilitiesKHR av1_capabilities {};
    av1_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR;
    VkVideoDecodeH264CapabilitiesKHR h264_capabilities {};
    h264_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR;
    VkVideoDecodeH265CapabilitiesKHR h265_capabilities {};
    h265_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR;

    VkVideoProfileInfoKHR profile_info {};
    profile_info.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    profile_info.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    VkVideoDecodeCapabilitiesKHR decode_capabilities {};
    decode_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;

    // Only 4:2:0 profiles decode to the formats that export.
    u8 bit_depth = 8;
    switch (codec_id) {
    case CodecID::VP9: {
        u8 profile = 0;
        if (auto parameters = codec.vp9_parameters(); parameters.has_value()) {
            profile = parameters->profile;
            bit_depth = max(parameters->bit_depth, static_cast<u8>(8));
        }
        if (profile != 0 && profile != 2)
            return {};
        vp9_profile.stdProfile = static_cast<StdVideoVP9Profile>(profile);
        profile_info.pNext = &vp9_profile;
        profile_info.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR;
        decode_capabilities.pNext = &vp9_capabilities;
        break;
    }
    case CodecID::AV1: {
        u8 profile = 0;
        if (auto parameters = codec.av1_parameters(); parameters.has_value()) {
            profile = parameters->profile;
            bit_depth = max(parameters->bit_depth, static_cast<u8>(8));
        }
        if (profile != 0)
            return {};
        av1_profile.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;
        profile_info.pNext = &av1_profile;
        profile_info.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
        decode_capabilities.pNext = &av1_capabilities;
        break;
    }
    case CodecID::H264: {
        // Without a profile, ask for High, which can decode the other two.
        h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
        if (auto parameters = codec.h264_parameters(); parameters.has_value()) {
            switch (parameters->profile().value_or(Codecs::H264::Profile::High10)) {
            case Codecs::H264::Profile::Baseline:
                h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_BASELINE;
                break;
            case Codecs::H264::Profile::Main:
                h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
                break;
            case Codecs::H264::Profile::High:
                break;
            default:
                return {};
            }
        }
        h264_profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
        profile_info.pNext = &h264_profile;
        profile_info.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
        decode_capabilities.pNext = &h264_capabilities;
        break;
    }
    case CodecID::H265: {
        h265_profile.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
        if (auto parameters = codec.h265_parameters(); parameters.has_value()) {
            switch (parameters->profile().value_or(Codecs::H265::Profile::Main12)) {
            case Codecs::H265::Profile::Main:
                break;
            case Codecs::H265::Profile::Main10:
                h265_profile.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN_10;
                bit_depth = 10;
                break;
            default:
                return {};
            }
        }
        profile_info.pNext = &h265_profile;
        profile_info.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
        decode_capabilities.pNext = &h265_capabilities;
        break;
    }
    default:
        VERIFY_NOT_REACHED();
    }

    // Only 8 and 10 bit samples decode into the formats that export.
    if (bit_depth != 8 && bit_depth != 10)
        return {};
    auto component_bit_depth = component_bit_depth_for(bit_depth);
    if (!component_bit_depth.has_value())
        return {};
    profile_info.lumaBitDepth = *component_bit_depth;
    profile_info.chromaBitDepth = *component_bit_depth;

    if (!device->decodes_codec_operation(profile_info.videoCodecOperation))
        return {};
    VkVideoCapabilitiesKHR video_capabilities {};
    video_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
    video_capabilities.pNext = &decode_capabilities;
    if (device->video_capabilities(profile_info, video_capabilities).is_error())
        return {};

    // Decoded frames are exported as dma-bufs, but a driver may not support any modifiers for export. Check whether
    // it does to determine whether export may succeed.
    auto output_format = bit_depth == 10 ? VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 : VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    if (checks_exportable_format == ChecksExportableFormat::Yes && device->exportable_drm_format_modifiers(output_format, VulkanExportableImagePool::IMAGE_USAGE, 0, nullptr, {}).is_empty())
        return {};

    // Before FFmpeg 7.1, a device that decodes references into the output images, but cannot keep each reference in an
    // image of its own, has FFmpeg use an array of reference images that it never created.
    auto decodes_references_into_the_output = (decode_capabilities.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) != 0;
    auto keeps_references_in_separate_images = (video_capabilities.flags & VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR) != 0;
    if (functions.avcodec_version() < AV_VERSION_INT(61, 19, 100) && decodes_references_into_the_output && !keeps_references_in_separate_images)
        return {};

    return DecoderCapabilities { .smooth = true, .power_efficient = true };
}

DecoderErrorOr<NonnullOwnPtr<FFmpegVulkanVideoDecoderImplementation>> FFmpegVulkanVideoDecoderImplementation::try_create(FFmpegFunctions const& functions, ExportsDirectly exports_directly, CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    auto* device = VulkanVideoDevice::the();
    if (device == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "No Vulkan device can decode video in this process"sv);
    // The checks that depend on the stream's profile use the one its configuration names, if it has one.
    auto codec = parse_configuration_record(codec_id, codec_initialization_data).value_or(ParsedCodec { codec_id });
    if (!capabilities(functions, codec, ChecksExportableFormat::No).has_value())
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "The Vulkan device cannot decode codec {} with this FFmpeg", codec_id);

    auto* device_context = TRY(device_context_for(functions, *device));
    if (vulkan_capable_decoder(functions, codec_id) == nullptr)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "FFmpeg cannot decode codec {} through Vulkan", codec_id);
    auto codec_configuration = DECODER_TRY_ALLOC(FixedArray<u8>::create(codec_initialization_data));

    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    ArmedScopeGuard memory_guard {
        [&] {
            functions.av_packet_free(&packet);
            functions.av_frame_free(&frame);
        }
    };

    packet = functions.av_packet_alloc();
    if (packet == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg packet"sv);

    frame = functions.av_frame_alloc();
    if (frame == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg frame"sv);

    auto decoder = DECODER_TRY_ALLOC(adopt_nonnull_own_or_enomem(new (nothrow) FFmpegVulkanVideoDecoderImplementation(functions, exports_directly, *device, codec_id, device_context, move(codec_configuration), packet, frame)));
    memory_guard.disarm();
    decoder->m_codec_context = TRY(decoder->create_codec_context());

    decoder->m_completion_thread = DECODER_TRY_ALLOC(Threading::Thread::try_create("VulkanDecode"sv, [decoder = decoder.ptr()] -> intptr_t {
        decoder->complete_frames_until_exit();
        return 0;
    }));
    decoder->m_completion_thread->start();
    return decoder;
}

FFmpegVulkanVideoDecoderImplementation::FFmpegVulkanVideoDecoderImplementation(FFmpegFunctions const& functions, ExportsDirectly exports_directly, VulkanVideoDevice& device, CodecID codec_id, AVBufferRef* device_context, FixedArray<u8> codec_configuration, AVPacket* packet, AVFrame* received_frame)
    : m_functions(functions)
    , m_exports_directly(exports_directly)
    , m_device(device)
    , m_codec_id(codec_id)
    , m_device_context(device_context)
    , m_packet(packet)
    , m_received_frame(received_frame)
    , m_frames_awaiting_output(codec_id, codec_configuration.span())
    , m_codec_configuration(move(codec_configuration))
{
}

DecoderErrorOr<AVCodecContext*> FFmpegVulkanVideoDecoderImplementation::create_codec_context()
{
    auto const* codec = vulkan_capable_decoder(m_functions, m_codec_id);
    VERIFY(codec != nullptr);

    auto* codec_context = m_functions.avcodec_alloc_context3(codec);
    if (codec_context == nullptr)
        return DecoderError::format(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg codec context for codec {}", m_codec_id);
    ArmedScopeGuard free_codec_context = [&] { m_functions.avcodec_free_context(&codec_context); };

    TRY(set_codec_initialization_data(m_functions, *codec_context, m_codec_id, m_codec_configuration.span()));
    // The device already decodes several frames at a time, and frame threading would only hold frames back.
    VERIFY(m_functions.av_opt_set_int(codec_context, "threads", 1, 0) == 0);
    codec_context->max_pixels = MAX_VIDEO_FRAME_AREA;
    codec_context->hw_device_ctx = m_functions.av_buffer_ref(m_device_context);
    if (codec_context->hw_device_ctx == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to reference the FFmpeg Vulkan device context"sv);
    codec_context->get_format = negotiate_pixel_format;
    codec_context->get_buffer2 = get_buffer;
    codec_context->opaque = this;
#ifdef AV_HWACCEL_FLAG_LADYBIRD_CLEAR_UNUSED_REF_FRAME_MVS
    // Only the bundled FFmpeg decodes AV1, and it is patched to know this flag.
    if (m_codec_id == CodecID::AV1 && !PlatformFeatures::ProcessFeatures::the().is_enabled(PlatformFeatures::Feature::VulkanVideoBundledFFmpegAV1KeepSequenceHeaderEnableRefMVsFlag))
        codec_context->hwaccel_flags |= AV_HWACCEL_FLAG_LADYBIRD_CLEAR_UNUSED_REF_FRAME_MVS;
#endif

    if (m_functions.avcodec_open2(codec_context, codec, nullptr) < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Unknown error occurred when opening FFmpeg codec {}", m_codec_id);

    free_codec_context.disarm();
    return codec_context;
}

DecoderErrorOr<void> FFmpegVulkanVideoDecoderImplementation::replace_codec_context()
{
    // The frames the old context still holds back for reordering come out before the new one starts.
    m_packet->data = nullptr;
    m_packet->size = 0;
    m_packet->pts = 0;
    m_packet->dts = 0;
    if (m_functions.avcodec_send_packet(m_codec_context, m_packet) == 0)
        TRY(receive_decoded_frames(EndOfStreamIsFinal::No));

    auto* codec_context = TRY(create_codec_context());
    m_functions.avcodec_free_context(&m_codec_context);
    m_functions.av_buffer_unref(&m_codec_frames_context);
    m_codec_context = codec_context;
    m_codec_context_has_negotiated = false;
    m_codec_context_must_be_replaced = false;
    return {};
}

FFmpegVulkanVideoDecoderImplementation::~FFmpegVulkanVideoDecoderImplementation()
{
    Vector<DecodingFrame> decoding_frames;
    {
        MutexLocker locker { m_mutex };
        m_completion_thread_should_exit = true;
        m_condition.broadcast();
    }
    if (m_completion_thread)
        (void)m_completion_thread->join();
    while (!m_decoding_frames.is_empty())
        decoding_frames.append(m_decoding_frames.dequeue());
    free_decoding_frames(decoding_frames);
    m_outputs.clear();

    // A stalled copy may still be pending on its command buffer.
    if (m_copy_command_pool != VK_NULL_HANDLE && !m_copy_stalled)
        vkDestroyCommandPool(m_device.logical_device(), m_copy_command_pool, nullptr);
    m_surfaces_by_image.clear();
    m_functions.av_buffer_unref(&m_frames_context_of_surfaces);

    m_functions.av_packet_free(&m_packet);
    m_functions.av_frame_free(&m_received_frame);
    m_functions.avcodec_free_context(&m_codec_context);
    m_functions.av_buffer_unref(&m_codec_frames_context);
}

AVPixelFormat FFmpegVulkanVideoDecoderImplementation::negotiate_pixel_format(AVCodecContext* codec_context, AVPixelFormat const* pixel_formats)
{
    auto& decoder = *static_cast<FFmpegVulkanVideoDecoderImplementation*>(codec_context->opaque);
    if (decoder.m_codec_context_has_negotiated) {
        decoder.m_codec_context_must_be_replaced = true;
        return AV_PIX_FMT_NONE;
    }
    for (auto const* pixel_format = pixel_formats; *pixel_format != AV_PIX_FMT_NONE; pixel_format++) {
        if (*pixel_format != AV_PIX_FMT_VULKAN)
            continue;
        auto result = decoder.create_frames_context();
        if (result.is_error()) {
            decoder.m_pixel_format_error = result.release_error();
            return AV_PIX_FMT_NONE;
        }
        decoder.m_codec_context_has_negotiated = true;
        return AV_PIX_FMT_VULKAN;
    }
    decoder.m_pixel_format_error = DecoderError::with_description(DecoderErrorCategory::NotImplemented, "FFmpeg cannot decode this stream through Vulkan"sv);
    return AV_PIX_FMT_NONE;
}

int FFmpegVulkanVideoDecoderImplementation::get_buffer(AVCodecContext* codec_context, AVFrame* frame, int flags)
{
    auto& decoder = *static_cast<FFmpegVulkanVideoDecoderImplementation*>(codec_context->opaque);

    // FFmpeg's AV1 decoder keeps its frames across a new sequence with larger frames, which would decode out of bounds.
    if (codec_context->hw_frames_ctx != nullptr) {
        auto const& frames_context = *reinterpret_cast<AVHWFramesContext const*>(codec_context->hw_frames_ctx->data);
        if (frame->width > frames_context.width || frame->height > frames_context.height) {
            decoder.m_codec_context_must_be_replaced = true;
            return AVERROR(EINVAL);
        }
    }

    return decoder.m_functions.avcodec_default_get_buffer2(codec_context, frame, flags);
}

DecoderErrorOr<void> FFmpegVulkanVideoDecoderImplementation::create_frames_context()
{
    AVBufferRef* frames_reference = nullptr;
    if (m_functions.avcodec_get_hw_frames_parameters(m_codec_context, m_codec_context->hw_device_ctx, AV_PIX_FMT_VULKAN, &frames_reference) < 0)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "FFmpeg cannot describe Vulkan frames for this stream"sv);
    ArmedScopeGuard free_frames_reference = [&] { m_functions.av_buffer_unref(&frames_reference); };

    auto& frames_context = *reinterpret_cast<AVHWFramesContext*>(frames_reference->data);
    auto& vulkan_frames_context = *static_cast<AVVulkanFramesContext*>(frames_context.hwctx);
    if (frames_context.sw_format != AV_PIX_FMT_NV12 && frames_context.sw_format != AV_PIX_FMT_P010)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "The stream decodes to a format that cannot be exported"sv);

    // Storage use is only for FFmpeg's own filters, and can rule out the modifiers that export.
    vulkan_frames_context.usage = static_cast<VkImageUsageFlagBits>(vulkan_frames_context.usage & ~VK_IMAGE_USAGE_STORAGE_BIT);

    // Decoded images that no DRM format modifier can export are copied into images that can.
    m_drm_format_modifiers = {};
    if (m_exports_directly == ExportsDirectly::Yes)
        m_drm_format_modifiers = m_device.exportable_drm_format_modifiers(vulkan_frames_context.format[0], vulkan_frames_context.usage, vulkan_frames_context.img_flags, vulkan_frames_context.create_pnext, Gfx::IntSize { frames_context.width, frames_context.height });
    if (!m_drm_format_modifiers.is_empty()) {
        // FFmpeg chains the video profile in, and leaves choosing a modifier to us.
        m_drm_format_modifier_list = {};
        m_drm_format_modifier_list.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
        m_drm_format_modifier_list.pNext = vulkan_frames_context.create_pnext;
        m_drm_format_modifier_list.drmFormatModifierCount = m_drm_format_modifiers.size();
        m_drm_format_modifier_list.pDrmFormatModifiers = m_drm_format_modifiers.data();
        vulkan_frames_context.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        vulkan_frames_context.create_pnext = &m_drm_format_modifier_list;
    }

    if (m_functions.av_hwframe_ctx_init(frames_reference) < 0)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "FFmpeg could not create Vulkan frames"sv);

    VERIFY(m_codec_frames_context == nullptr);
    m_codec_frames_context = m_functions.av_buffer_ref(frames_reference);
    if (m_codec_frames_context == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Could not reference the decoded frames' context"sv);
    m_functions.av_buffer_unref(&m_codec_context->hw_frames_ctx);
    m_codec_context->hw_frames_ctx = frames_reference;
    free_frames_reference.disarm();
    return {};
}

DecoderErrorOr<void> FFmpegVulkanVideoDecoderImplementation::receive_coded_data(CodedFrame const& coded_frame, DecodeIntent intent)
{
    if (m_pixel_format_error.has_value())
        return *m_pixel_format_error;

    if (auto configuration = coded_frame.new_codec_configuration(); configuration.has_value() && !configuration->is_empty())
        m_codec_configuration = DECODER_TRY_ALLOC(FixedArray<u8>::create(*configuration));

    auto key = TRY(m_frames_awaiting_output.add(coded_frame, intent));
    auto result = decode_coded_frame(coded_frame, key);
    if (!m_codec_context_must_be_replaced)
        return result;

    // A new context lacks the earlier frames, so only a keyframe can start it.
    if (!coded_frame.is_keyframe())
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "The stream changes its format on a frame other than a keyframe"sv);

    TRY(replace_codec_context());
    result = decode_coded_frame(coded_frame, key);
    if (result.is_error() && result.error().category() == DecoderErrorCategory::NotImplemented)
        return result;
    if (result.is_error() || m_codec_context_must_be_replaced)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "The stream changes its format on a keyframe that a new decoder cannot start from"sv);
    return {};
}

DecoderErrorOr<void> FFmpegVulkanVideoDecoderImplementation::decode_coded_frame(CodedFrame const& coded_frame, u64 key)
{
    auto result = send_coded_frame(m_functions, *m_codec_context, *m_packet, coded_frame, key);
    // FFmpeg takes no more until the frames it has decoded are taken out.
    if (result.is_error() && result.error().category() == DecoderErrorCategory::TryAgain) {
        TRY(receive_decoded_frames());
        result = send_coded_frame(m_functions, *m_codec_context, *m_packet, coded_frame, key);
    }
    if (result.is_error() && m_pixel_format_error.has_value())
        return *m_pixel_format_error;
    TRY(result);

    return receive_decoded_frames();
}

void FFmpegVulkanVideoDecoderImplementation::signal_end_of_stream()
{
    m_packet->data = nullptr;
    m_packet->size = 0;
    m_packet->pts = 0;
    m_packet->dts = 0;

    auto result = m_functions.avcodec_send_packet(m_codec_context, m_packet);
    VERIFY(result == 0 || result == AVERROR_EOF);
    if (auto received = receive_decoded_frames(); received.is_error()) {
        MutexLocker locker { m_mutex };
        if (!m_decode_failure.has_value())
            m_decode_failure = received.release_error();
    }
}

DecoderErrorOr<void> FFmpegVulkanVideoDecoderImplementation::receive_decoded_frames(EndOfStreamIsFinal end_of_stream_is_final)
{
    while (true) {
        auto result = m_functions.avcodec_receive_frame(m_codec_context, m_received_frame);
        if (result == AVERROR(EAGAIN))
            return {};
        if (result == AVERROR_EOF) {
            if (end_of_stream_is_final == EndOfStreamIsFinal::Yes && !m_received_end_of_stream) {
                m_received_end_of_stream = true;
                DecodingFrame end_of_stream;
                end_of_stream.generation = m_generation;
                enqueue_decoding_frame(move(end_of_stream));
            }
            return {};
        }
        if (result < 0) {
            if (m_pixel_format_error.has_value())
                return *m_pixel_format_error;
            return DecoderError::format(DecoderErrorCategory::Unknown, "FFmpeg codec encountered an unexpected error retrieving frames with code {:x}", result);
        }

        auto timing = m_frames_awaiting_output.take(m_received_frame->pts);
        if (!timing.has_value() || timing->intent == DecodeIntent::Reference) {
            m_functions.av_frame_unref(m_received_frame);
            continue;
        }

        auto* frame = m_functions.av_frame_alloc();
        if (frame == nullptr) {
            m_functions.av_frame_unref(m_received_frame);
            return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg frame"sv);
        }
        m_functions.av_frame_move_ref(frame, m_received_frame);

        DecodingFrame decoding_frame;
        decoding_frame.frame = frame;
        decoding_frame.timing = *timing;
        decoding_frame.cicp = coding_independent_code_points_of(m_functions, *m_codec_context, {});
        decoding_frame.generation = m_generation;
        auto decoded = semaphore_value_of(*frame);
        decoding_frame.semaphore = decoded.semaphore;
        decoding_frame.decoded_semaphore_value = decoded.value;
        enqueue_decoding_frame(move(decoding_frame));
    }
}

void FFmpegVulkanVideoDecoderImplementation::enqueue_decoding_frame(DecodingFrame decoding_frame)
{
    MutexLocker locker { m_mutex };
    m_decoding_frames.enqueue(move(decoding_frame));
    m_condition.broadcast();
}

void FFmpegVulkanVideoDecoderImplementation::free_decoding_frames(Vector<DecodingFrame>& decoding_frames)
{
    for (auto& decoding_frame : decoding_frames)
        m_functions.av_frame_free(&decoding_frame.frame);
    decoding_frames.clear();
}

void FFmpegVulkanVideoDecoderImplementation::complete_frames_until_exit()
{
    while (true) {
        DecodingFrame decoding_frame;
        {
            MutexLocker locker { m_mutex };
            while (m_decoding_frames.is_empty() && !m_completion_thread_should_exit)
                m_condition.wait();
            if (m_completion_thread_should_exit)
                return;
            decoding_frame = m_decoding_frames.dequeue();
            m_frame_is_completing = true;
        }

        Optional<DecoderErrorOr<DecodedOutput>> output;
        if (decoding_frame.frame != nullptr)
            output = complete_frame(decoding_frame);

        MutexLocker locker { m_mutex };
        m_frame_is_completing = false;
        auto completion = DecodeCompletion::ProducedNothing;
        if (decoding_frame.generation == m_generation) {
            if (!output.has_value())
                m_completed_end_of_stream = true;
            else if (output->is_error())
                m_decode_failure = output->release_error();
            else
                m_outputs.append(output->release_value());
            completion = DecodeCompletion::ProducedOutput;
        }
        auto caller_needs_waking = completion == DecodeCompletion::ProducedOutput || m_decoding_frames.is_empty();
        if (caller_needs_waking && !m_caller_is_waiting_for_output && m_decode_completed_callback)
            m_decode_completed_callback(completion);
        m_condition.broadcast();
    }
}

DecoderErrorOr<FFmpegVulkanVideoDecoderImplementation::DecodedOutput> FFmpegVulkanVideoDecoderImplementation::complete_frame(DecodingFrame& decoding_frame)
{
    ScopeGuard free_frame = [&] { m_functions.av_frame_free(&decoding_frame.frame); };
    auto& frame = *decoding_frame.frame;

    if (!wait_for_semaphore(m_device.logical_device(), decoding_frame.semaphore, decoding_frame.decoded_semaphore_value))
        return DecoderError::with_description(DecoderErrorCategory::Unknown, "The Vulkan device failed or stalled while decoding a frame"sv);

    auto const& frames_context = frames_context_of(frame);
    Gfx::Size<u32> size { static_cast<u32>(frame.width), static_cast<u32>(frame.height) };
    auto bit_depth = bit_depth_of(frames_context);

    // The decoded image goes back to be decoded into as soon as it has been copied.
    if (static_cast<AVVulkanFramesContext const*>(frames_context.hwctx)->tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
        auto image = TRY(copy_into_exportable_image(frame));
        return DecodedOutput { image->surface(), move(image), decoding_frame.timing, size, bit_depth, decoding_frame.cicp };
    }

    auto surface = TRY(surface_for_frame(frame));
    auto contents = DECODER_TRY_ALLOC(try_make_ref_counted<VulkanFrameContents>(m_functions, decoding_frame.frame));
    decoding_frame.frame = nullptr;
    return DecodedOutput { move(surface), move(contents), decoding_frame.timing, size, bit_depth, decoding_frame.cicp };
}

DecoderErrorOr<void> FFmpegVulkanVideoDecoderImplementation::prepare_to_copy_frames_from(AVFrame const& frame)
{
    auto* logical_device = m_device.logical_device();
    if (m_copy_command_pool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo pool_info {};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = m_device.compute_queue_family();
        if (vkCreateCommandPool(logical_device, &pool_info, nullptr, &m_copy_command_pool) != VK_SUCCESS)
            return DecoderError::with_description(DecoderErrorCategory::Memory, "Could not create a command pool for copying decoded frames"sv);
    }
    if (m_copy_command_buffer == VK_NULL_HANDLE) {
        VkCommandBufferAllocateInfo buffer_info {};
        buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        buffer_info.commandPool = m_copy_command_pool;
        buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        buffer_info.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(logical_device, &buffer_info, &m_copy_command_buffer) != VK_SUCCESS)
            return DecoderError::with_description(DecoderErrorCategory::Memory, "Could not allocate a command buffer for copying decoded frames"sv);
    }

    auto const& frames_context = frames_context_of(frame);
    auto const& vulkan_frames_context = *static_cast<AVVulkanFramesContext const*>(frames_context.hwctx);
    if (vulkan_frames_context.format[1] != VK_FORMAT_UNDEFINED)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Decoded frames that span several images cannot be copied"sv);

    auto format = vulkan_frames_context.format[0];
    Gfx::IntSize size { frames_context.width, frames_context.height };
    if (m_exportable_images != nullptr && m_exportable_images->format() == format && m_exportable_images->size() == size)
        return {};
    auto drm_format = frames_context.sw_format == AV_PIX_FMT_P010 ? DRM_FORMAT_P010 : DRM_FORMAT_NV12;
    auto exportable_images = VulkanExportableImagePool::create(m_device, format, size, drm_format);
    if (exportable_images.is_error())
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Decoded frames cannot be copied for export: {}", exportable_images.error());
    m_exportable_images = exportable_images.release_value();
    return {};
}

DecoderErrorOr<NonnullRefPtr<VulkanExportableImagePool::Image>> FFmpegVulkanVideoDecoderImplementation::copy_into_exportable_image(AVFrame const& frame)
{
    if (m_copy_stalled)
        return DecoderError::with_description(DecoderErrorCategory::Unknown, "The Vulkan device stalled while copying an earlier frame"sv);
    TRY(prepare_to_copy_frames_from(frame));

    auto image_or_error = m_exportable_images->take_image();
    if (image_or_error.is_error())
        return DecoderError::format(DecoderErrorCategory::Memory, "Could not allocate an exportable image: {}", image_or_error.error());
    auto destination = image_or_error.release_value();

    auto& source_context = frames_context_of(frame);
    auto& source_vulkan_context = *static_cast<AVVulkanFramesContext*>(source_context.hwctx);
    auto* source = reinterpret_cast<AVVkFrame*>(frame.data[0]);

    // FFmpeg may be decoding other frames that use the image as a reference, and orders each use of it by the values of
    // its semaphore, under the frame's lock. Taking the next value under the same lock places the copy between them.
    source_vulkan_context.lock_frame(&source_context, source);
    ArmedScopeGuard unlock_source = [&] { source_vulkan_context.unlock_frame(&source_context, source); };

    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(m_copy_command_buffer, &begin_info) != VK_SUCCESS)
        return DecoderError::with_description(DecoderErrorCategory::Unknown, "Could not begin recording a frame copy"sv);

    VkImageSubresourceRange const whole_image { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier2 prepare_barriers[2] {};
    prepare_barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    prepare_barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    prepare_barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    prepare_barriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    prepare_barriers[0].oldLayout = source->layout[0];
    prepare_barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    prepare_barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prepare_barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prepare_barriers[0].image = source->img[0];
    prepare_barriers[0].subresourceRange = whole_image;

    // The image's previous contents are discarded, so whichever device last read it needs no handing back.
    prepare_barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    prepare_barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    prepare_barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    prepare_barriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    prepare_barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    prepare_barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    prepare_barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prepare_barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prepare_barriers[1].image = destination->image();
    prepare_barriers[1].subresourceRange = whole_image;

    VkDependencyInfo prepare_dependency {};
    prepare_dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    prepare_dependency.imageMemoryBarrierCount = 2;
    prepare_dependency.pImageMemoryBarriers = prepare_barriers;
    vkCmdPipelineBarrier2(m_copy_command_buffer, &prepare_dependency);

    VkImageCopy regions[2] {};
    regions[0].srcSubresource = { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 };
    regions[0].dstSubresource = { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 };
    regions[0].extent = { static_cast<u32>(source_context.width), static_cast<u32>(source_context.height), 1 };
    regions[1].srcSubresource = { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 };
    regions[1].dstSubresource = { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 };
    regions[1].extent = { static_cast<u32>(source_context.width + 1) / 2, static_cast<u32>(source_context.height + 1) / 2, 1 };
    vkCmdCopyImage(m_copy_command_buffer, source->img[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, regions);

    // The copy leaves the image ready for whichever device samples it next. It is shared between the queue families
    // whenever there are several, and is otherwise owned by the one family.
    auto releasing_queue_family = m_device.queue_families().size() > 1 ? VK_QUEUE_FAMILY_IGNORED : m_device.compute_queue_family();
    VkImageMemoryBarrier2 export_barrier {};
    export_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    export_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    export_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    export_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    export_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    export_barrier.srcQueueFamilyIndex = releasing_queue_family;
    export_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    export_barrier.image = destination->image();
    export_barrier.subresourceRange = whole_image;

    VkDependencyInfo export_dependency {};
    export_dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    export_dependency.imageMemoryBarrierCount = 1;
    export_dependency.pImageMemoryBarriers = &export_barrier;
    vkCmdPipelineBarrier2(m_copy_command_buffer, &export_dependency);

    if (vkEndCommandBuffer(m_copy_command_buffer) != VK_SUCCESS)
        return DecoderError::with_description(DecoderErrorCategory::Unknown, "Could not finish recording a frame copy"sv);

    auto semaphore = source->sem[0];
    auto copied_semaphore_value = source->sem_value[0] + 1;

    VkSemaphoreSubmitInfo wait_info {};
    wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait_info.semaphore = semaphore;
    wait_info.value = source->sem_value[0];
    wait_info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSemaphoreSubmitInfo signal_info {};
    signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal_info.semaphore = semaphore;
    signal_info.value = copied_semaphore_value;
    signal_info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo command_buffer_info {};
    command_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command_buffer_info.commandBuffer = m_copy_command_buffer;

    VkSubmitInfo2 submit_info {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit_info.waitSemaphoreInfoCount = 1;
    submit_info.pWaitSemaphoreInfos = &wait_info;
    submit_info.commandBufferInfoCount = 1;
    submit_info.pCommandBufferInfos = &command_buffer_info;
    submit_info.signalSemaphoreInfoCount = 1;
    submit_info.pSignalSemaphoreInfos = &signal_info;

    auto logical_device = m_device.logical_device();
    auto queue_family = m_device.compute_queue_family();
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(logical_device, queue_family, 0, &queue);
    m_device.lock_queue(queue_family);
    auto submit_result = vkQueueSubmit2(queue, 1, &submit_info, VK_NULL_HANDLE);
    m_device.unlock_queue(queue_family);
    if (submit_result != VK_SUCCESS)
        return DecoderError::with_description(DecoderErrorCategory::Unknown, "Could not submit a frame copy"sv);

    // FFmpeg's next use of the image waits for the copy, and moves the image on from the layout the copy left it in.
    source->sem_value[0] = copied_semaphore_value;
    source->layout[0] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    source->access[0] = static_cast<RemoveReference<decltype(source->access[0])>>(VK_ACCESS_2_TRANSFER_READ_BIT);
    unlock_source.disarm();
    source_vulkan_context.unlock_frame(&source_context, source);

    // The command buffer is reused for the next copy. A copy that never finishes keeps using its images.
    if (!wait_for_semaphore(logical_device, semaphore, copied_semaphore_value)) {
        m_copy_stalled = true;
        destination->abandon();
        return DecoderError::with_description(DecoderErrorCategory::Unknown, "The Vulkan device failed or stalled while copying a decoded frame"sv);
    }

    return destination;
}

DecoderErrorOr<NonnullRefPtr<VideoSurface>> FFmpegVulkanVideoDecoderImplementation::surface_for_frame(AVFrame const& frame)
{
    if (m_frames_context_of_surfaces == nullptr || m_frames_context_of_surfaces->data != frame.hw_frames_ctx->data) {
        m_surfaces_by_image.clear();
        m_functions.av_buffer_unref(&m_frames_context_of_surfaces);
        m_frames_context_of_surfaces = m_functions.av_buffer_ref(frame.hw_frames_ctx);
        if (m_frames_context_of_surfaces == nullptr)
            return DecoderError::with_description(DecoderErrorCategory::Memory, "Could not reference the exported frames' context"sv);
    }
    auto const& frames_context = frames_context_of(frame);

    auto const& vulkan_frame = *reinterpret_cast<AVVkFrame const*>(frame.data[0]);
    u64 image_key = 0;
    static_assert(sizeof(vulkan_frame.img[0]) <= sizeof(image_key));
    memcpy(&image_key, &vulkan_frame.img[0], sizeof(vulkan_frame.img[0]));
    if (auto surface = m_surfaces_by_image.get(image_key); surface.has_value())
        return NonnullRefPtr { **surface };

    // The image is exported once, however many frames are decoded into it.
    if (vulkan_frame.img[1] != VK_NULL_HANDLE || vulkan_frame.mem[1] != VK_NULL_HANDLE)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Decoded frames that span several images cannot be exported"sv);
    auto format = static_cast<AVVulkanFramesContext const*>(frames_context.hwctx)->format[0];
    auto drm_format = frames_context.sw_format == AV_PIX_FMT_P010 ? DRM_FORMAT_P010 : DRM_FORMAT_NV12;
    auto dma_buf = m_device.export_image_as_dma_buf(vulkan_frame.img[0], vulkan_frame.mem[0], format, { frames_context.width, frames_context.height }, drm_format);
    if (dma_buf.is_error())
        return DecoderError::format(DecoderErrorCategory::Unknown, "Could not export a decoded image: {}", dma_buf.error());

    auto surface = VideoSurface::create_from_dma_buf(get_random<u64>(), dma_buf.release_value());
    if (surface.is_error())
        return DecoderError::format(DecoderErrorCategory::Memory, "Could not create a surface for a decoded image: {}", surface.error());
    DECODER_TRY_ALLOC(m_surfaces_by_image.try_set(image_key, surface.value()));
    return surface.release_value();
}

DecoderErrorOr<NonnullRefPtr<VideoFrame>> FFmpegVulkanVideoDecoderImplementation::take_next_output(ColorMetadata const& container_color_metadata, Optional<AK::Duration>)
{
    TRY(receive_decoded_frames());

    MutexLocker locker { m_mutex };
    while (m_outputs.is_empty()) {
        if (m_decode_failure.has_value())
            return m_decode_failure.release_value();
        if (m_completed_end_of_stream)
            return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "FFmpeg decoder has been flushed"sv);
        // Frames still being completed wake the caller when they come out, unless it is draining, when nothing else
        // can arrive to do so.
        if (!m_received_end_of_stream)
            return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "The Vulkan device has no finished frame to hand out"sv);

        m_caller_is_waiting_for_output = true;
        m_condition.wait();
        m_caller_is_waiting_for_output = false;
    }

    auto const& output = m_outputs.first();
    auto storage = m_surface_budget->try_hold(output.surface, output.contents);
    if (!storage)
        return DecoderError::with_description(DecoderErrorCategory::TryAgain, "Every video surface the decoder may hand out is held elsewhere"sv);

    auto color_metadata = container_color_metadata;
    color_metadata.adopt_specified_values(output.timing.high_dynamic_range_metadata);
    color_metadata.cicp.adopt_specified_values(output.cicp);
    auto frame = DECODER_TRY_ALLOC(try_make_ref_counted<VideoFrame>(output.timing.timestamp, output.timing.duration, output.size, output.bit_depth, Subsampling::yuv420(), move(color_metadata), storage.release_nonnull()));
    m_outputs.take_first();
    return frame;
}

void FFmpegVulkanVideoDecoderImplementation::flush()
{
    m_functions.avcodec_flush_buffers(m_codec_context);
    m_frames_awaiting_output.flush();
    m_received_end_of_stream = false;

    Vector<DecodingFrame> stale_frames;
    Vector<DecodedOutput> stale_outputs;
    {
        MutexLocker locker { m_mutex };
        m_generation++;
        while (!m_decoding_frames.is_empty())
            stale_frames.append(m_decoding_frames.dequeue());
        stale_outputs = move(m_outputs);
        m_decode_failure.clear();
        m_completed_end_of_stream = false;
    }
    free_decoding_frames(stale_frames);
}

namespace {

class Backend final : public VulkanVideoDecoderBackend {
public:
    AK_ALLOC_WITH_KMALLOC;

    Backend(FFmpegFunctions const& functions, PlatformFeatures::Feature direct_export_feature)
        : m_functions(functions)
        , m_direct_export_feature(direct_export_feature)
    {
    }

    virtual Optional<DecoderCapabilities> capabilities(ParsedCodec const& codec) const override
    {
        return FFmpegVulkanVideoDecoderImplementation::capabilities(m_functions, codec);
    }

    virtual DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> try_create(CodecID codec_id, ReadonlyBytes codec_initialization_data) const override
    {
        // Before libavcodec 63, FFmpeg creates its images with other usage and flags than it is asked for, which the
        // DRM format modifiers chosen for them may not allow.
        auto exports_directly = FFmpegVulkanVideoDecoderImplementation::ExportsDirectly::No;
        if (LIBAVCODEC_VERSION_MAJOR >= 63 && PlatformFeatures::ProcessFeatures::the().is_enabled(m_direct_export_feature))
            exports_directly = FFmpegVulkanVideoDecoderImplementation::ExportsDirectly::Yes;
        return NonnullOwnPtr<VideoDecoder> { TRY(FFmpegVulkanVideoDecoderImplementation::try_create(m_functions, exports_directly, codec_id, codec_initialization_data)) };
    }

private:
    FFmpegFunctions const& m_functions;
    PlatformFeatures::Feature m_direct_export_feature;
};

// Whether the library lays its structs out as the headers this was compiled against do.
bool library_matches_these_headers(FFmpegFunctions const& functions)
{
    if (AV_VERSION_MAJOR(functions.avcodec_version()) != LIBAVCODEC_VERSION_MAJOR || AV_VERSION_MAJOR(functions.avutil_version()) != LIBAVUTIL_VERSION_MAJOR)
        return false;
    // FFmpeg 6.0 shares its major versions with 6.1, which changed the Vulkan structs to decode video.
    if (LIBAVUTIL_VERSION_MAJOR == 58 && functions.avutil_version() < AV_VERSION_INT(58, 29, 100))
        return false;

    auto* codec_context = functions.avcodec_alloc_context3(nullptr);
    if (codec_context == nullptr)
        return false;
    auto negotiates_formats_where_expected = codec_context->get_format == functions.avcodec_default_get_format;
    functions.avcodec_free_context(&codec_context);
    return negotiates_formats_where_expected;
}

}

#ifdef LADYBIRD_SYSTEM_FFMPEG_NAMESPACE

}

namespace Media::FFmpeg {

template<>
VulkanVideoDecoderBackend const* vulkan_video_decoder_backend_for_library<LIBAVCODEC_VERSION_MAJOR>(void* library_handle)
{
    using namespace FFMPEG_ABI_NAMESPACE;
    static VulkanVideoDecoderBackend const* backend = [&]() -> VulkanVideoDecoderBackend const* {
        auto const* functions = FFMPEG_ABI_NAMESPACE::FFmpegFunctions::resolved_from_library(library_handle);
        if (functions == nullptr || !library_matches_these_headers(*functions))
            return nullptr;
        return new (nothrow) Backend(*functions, PlatformFeatures::Feature::VulkanVideoSystemFFmpegDirectExport);
    }();
    return backend;
}

#else

VulkanVideoDecoderBackend const& vulkan_video_decoder_backend_for_bundled_ffmpeg()
{
    static NeverDestroyed<Backend> backend { FFmpegFunctions::bundled(), PlatformFeatures::Feature::VulkanVideoBundledFFmpegDirectExport };
    return *backend;
}

VulkanVideoDecoderBackend const* vulkan_video_decoder_backend_for_system_ffmpeg(FFmpegFunctions const& functions)
{
    static VulkanVideoDecoderBackend const* backend = [&]() -> VulkanVideoDecoderBackend const* {
        if (!library_matches_these_headers(functions))
            return nullptr;
        return new (nothrow) Backend(functions, PlatformFeatures::Feature::VulkanVideoSystemFFmpegDirectExport);
    }();
    return backend;
}

#endif

}
