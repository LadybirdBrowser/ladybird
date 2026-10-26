/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Diagnostics.h>
#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <AK/ScopeGuard.h>
#include <LibIPC/File.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibMediaCodecs/FFmpeg/FFmpegFunctions.h>
#include <LibMediaCodecs/FFmpeg/FFmpegHelpers.h>
#include <LibMediaCodecs/FFmpeg/FFmpegVulkanVideoDecoder.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibThreading/Thread.h>
#include <string.h>

#if defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID)
#    include <LibMediaCodecs/FFmpeg/SystemFFmpeg.h>
#endif

extern "C" {
#include <libavutil/hwcontext_drm.h>
#include <libavutil/hwcontext_vulkan.h>
}

namespace Media::FFmpeg {

namespace {

constexpr u32 drm_fourcc(char a, char b, char c, char d)
{
    return static_cast<u32>(a) | (static_cast<u32>(b) << 8) | (static_cast<u32>(c) << 16) | (static_cast<u32>(d) << 24);
}

constexpr u32 DRM_FORMAT_NV12 = drm_fourcc('N', 'V', '1', '2');
constexpr u32 DRM_FORMAT_P010 = drm_fourcc('P', '0', '1', '0');

constexpr AK::Duration DEVICE_WORK_STALL_UNTIL_TREATED_AS_FAILED = AK::Duration::from_seconds(5);

// Surfaces are told apart by ID in every process that sees them, and each decoder's images are distinct.
Atomic<u32> s_next_surface_id { 1 };

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

// We only ship decoders for royalty-free codecs, so H.264 and HEVC decode through the system's FFmpeg. The decoder
// reaches into FFmpeg's hardware structs, so that library is only used when they are laid out as in the bundled one.
FFmpegFunctions const* ffmpeg_for(CodecID codec_id)
{
    switch (codec_id) {
    case CodecID::VP9:
    case CodecID::AV1:
        return &FFmpegFunctions::bundled();
#if defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID)
    case CodecID::H264:
    case CodecID::H265: {
        auto const* library = SystemFFmpeg::the();
        if (library == nullptr || !library->shares_bundled_struct_layouts())
            return nullptr;
        return &library->functions();
    }
#endif
    default:
        return nullptr;
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
    for (auto const& family : device.queue_families()) {
        auto& queue_family = vulkan_context.qf[vulkan_context.nb_qf++];
        queue_family.idx = static_cast<int>(family.index);
        queue_family.num = 1;
        queue_family.flags = static_cast<VkQueueFlagBits>(family.flags);
    }
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

Optional<DecoderCapabilities> FFmpegVulkanVideoDecoder::capabilities(ParsedCodec const& codec)
{
    auto* device = VulkanVideoDevice::the();
    if (device == nullptr)
        return {};

    auto codec_id = codec.codec_id();
    auto const* functions = ffmpeg_for(codec_id);
    if (functions == nullptr || !device->has_enabled_extension(decode_extension_for(codec_id)))
        return {};
    if (vulkan_capable_decoder(*functions, codec_id) == nullptr)
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

    VkVideoCapabilitiesKHR video_capabilities {};
    video_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
    video_capabilities.pNext = &decode_capabilities;
    if (device->video_capabilities(profile_info, video_capabilities).is_error())
        return {};

    return DecoderCapabilities { .smooth = true, .power_efficient = true };
}

DecoderErrorOr<NonnullOwnPtr<FFmpegVulkanVideoDecoder>> FFmpegVulkanVideoDecoder::try_create(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    auto* device = VulkanVideoDevice::the();
    if (device == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "No Vulkan device can decode video in this process"sv);
    auto const* ffmpeg = ffmpeg_for(codec_id);
    if (ffmpeg == nullptr || !device->has_enabled_extension(decode_extension_for(codec_id)))
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "The Vulkan device cannot decode codec {}", codec_id);
    auto const& functions = *ffmpeg;

    auto* device_context = TRY(device_context_for(functions, *device));

    AVCodecContext* codec_context = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    ArmedScopeGuard memory_guard {
        [&] {
            functions.avcodec_free_context(&codec_context);
            functions.av_packet_free(&packet);
            functions.av_frame_free(&frame);
        }
    };

    auto const* codec = vulkan_capable_decoder(functions, codec_id);
    if (codec == nullptr)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "FFmpeg cannot decode codec {} through Vulkan", codec_id);

    codec_context = functions.avcodec_alloc_context3(codec);
    if (codec_context == nullptr)
        return DecoderError::format(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg codec context for codec {}", codec_id);

    TRY(set_codec_initialization_data(functions, *codec_context, codec_id, codec_initialization_data));
    // The device already decodes several frames at a time, and frame threading would only hold frames back.
    VERIFY(functions.av_opt_set_int(codec_context, "threads", 1, 0) == 0);
    codec_context->hw_device_ctx = functions.av_buffer_ref(device_context);
    if (codec_context->hw_device_ctx == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to reference the FFmpeg Vulkan device context"sv);
    codec_context->get_format = negotiate_pixel_format;
    codec_context->get_buffer2 = get_buffer;

    if (functions.avcodec_open2(codec_context, codec, nullptr) < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Unknown error occurred when opening FFmpeg codec {}", codec_id);

    packet = functions.av_packet_alloc();
    if (packet == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg packet"sv);

    frame = functions.av_frame_alloc();
    if (frame == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg frame"sv);

    auto surface_pool = VideoFrameSurfacePool::create();
    if (surface_pool.is_error())
        return DecoderError::format(DecoderErrorCategory::Memory, "Failed to create a video surface pool: {}", surface_pool.release_error());

    memory_guard.disarm();
    auto decoder = DECODER_TRY_ALLOC(adopt_nonnull_own_or_enomem(new (nothrow) FFmpegVulkanVideoDecoder(functions, *device, codec_context, packet, frame, surface_pool.release_value())));
    codec_context->opaque = decoder.ptr();

    decoder->m_completion_thread = DECODER_TRY_ALLOC(Threading::Thread::try_create("VulkanDecode"sv, [decoder = decoder.ptr()] -> intptr_t {
        decoder->complete_frames_until_exit();
        return 0;
    }));
    decoder->m_completion_thread->start();
    return decoder;
}

FFmpegVulkanVideoDecoder::FFmpegVulkanVideoDecoder(FFmpegFunctions const& functions, VulkanVideoDevice& device, AVCodecContext* codec_context, AVPacket* packet, AVFrame* received_frame, NonnullRefPtr<VideoFrameSurfacePool> surface_pool)
    : m_functions(functions)
    , m_device(device)
    , m_codec_context(codec_context)
    , m_packet(packet)
    , m_received_frame(received_frame)
    , m_surface_pool(move(surface_pool))
{
}

FFmpegVulkanVideoDecoder::~FFmpegVulkanVideoDecoder()
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

    m_surface_pool->shed_storage();
    m_functions.av_packet_free(&m_packet);
    m_functions.av_frame_free(&m_received_frame);
    m_functions.avcodec_free_context(&m_codec_context);
}

AVPixelFormat FFmpegVulkanVideoDecoder::negotiate_pixel_format(AVCodecContext* codec_context, AVPixelFormat const* pixel_formats)
{
    auto& decoder = *static_cast<FFmpegVulkanVideoDecoder*>(codec_context->opaque);
    for (auto const* pixel_format = pixel_formats; *pixel_format != AV_PIX_FMT_NONE; pixel_format++) {
        if (*pixel_format != AV_PIX_FMT_VULKAN)
            continue;
        auto result = decoder.create_frames_context();
        if (result.is_error()) {
            decoder.m_pixel_format_error = result.release_error();
            return AV_PIX_FMT_NONE;
        }
        return AV_PIX_FMT_VULKAN;
    }
    decoder.m_pixel_format_error = DecoderError::with_description(DecoderErrorCategory::NotImplemented, "FFmpeg cannot decode this stream through Vulkan"sv);
    return AV_PIX_FMT_NONE;
}

int FFmpegVulkanVideoDecoder::get_buffer(AVCodecContext* codec_context, AVFrame* frame, int flags)
{
    auto& decoder = *static_cast<FFmpegVulkanVideoDecoder*>(codec_context->opaque);
    MutexLocker locker { decoder.m_frames_context_mutex };
    return decoder.m_functions.avcodec_default_get_buffer2(codec_context, frame, flags);
}

DecoderErrorOr<void> FFmpegVulkanVideoDecoder::create_frames_context()
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

    m_drm_format_modifiers = m_device.exportable_drm_format_modifiers(vulkan_frames_context.format[0], vulkan_frames_context.usage, vulkan_frames_context.img_flags, vulkan_frames_context.create_pnext);
    if (m_drm_format_modifiers.is_empty())
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "No DRM format modifier can export the decoded images"sv);

    // FFmpeg chains the video profile in, and leaves choosing a modifier to us.
    m_drm_format_modifier_list = {};
    m_drm_format_modifier_list.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    m_drm_format_modifier_list.pNext = vulkan_frames_context.create_pnext;
    m_drm_format_modifier_list.drmFormatModifierCount = m_drm_format_modifiers.size();
    m_drm_format_modifier_list.pDrmFormatModifiers = m_drm_format_modifiers.data();
    vulkan_frames_context.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    vulkan_frames_context.create_pnext = &m_drm_format_modifier_list;

    if (m_functions.av_hwframe_ctx_init(frames_reference) < 0)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "FFmpeg could not create exportable Vulkan frames"sv);

    m_functions.av_buffer_unref(&m_codec_context->hw_frames_ctx);
    m_codec_context->hw_frames_ctx = frames_reference;
    free_frames_reference.disarm();
    return {};
}

DecoderErrorOr<void> FFmpegVulkanVideoDecoder::receive_coded_data(CodedFrame const& coded_frame, DecodeIntent intent)
{
    if (m_pixel_format_error.has_value())
        return *m_pixel_format_error;

    auto key = TRY(m_frames_awaiting_output.add(coded_frame, intent));
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

void FFmpegVulkanVideoDecoder::signal_end_of_stream()
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

DecoderErrorOr<void> FFmpegVulkanVideoDecoder::receive_decoded_frames()
{
    while (true) {
        auto result = m_functions.avcodec_receive_frame(m_codec_context, m_received_frame);
        if (result == AVERROR(EAGAIN))
            return {};
        if (result == AVERROR_EOF) {
            if (!m_received_end_of_stream) {
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
        decoding_frame.generation = m_generation;
        auto decoded = semaphore_value_of(*frame);
        decoding_frame.semaphore = decoded.semaphore;
        decoding_frame.decoded_semaphore_value = decoded.value;
        enqueue_decoding_frame(move(decoding_frame));
    }
}

void FFmpegVulkanVideoDecoder::enqueue_decoding_frame(DecodingFrame decoding_frame)
{
    MutexLocker locker { m_mutex };
    m_decoding_frames.enqueue(move(decoding_frame));
    m_condition.broadcast();
}

void FFmpegVulkanVideoDecoder::free_decoding_frames(Vector<DecodingFrame>& decoding_frames)
{
    for (auto& decoding_frame : decoding_frames)
        m_functions.av_frame_free(&decoding_frame.frame);
    decoding_frames.clear();
}

void FFmpegVulkanVideoDecoder::complete_frames_until_exit()
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
        }

        Optional<DecoderErrorOr<DecodedOutput>> output;
        if (decoding_frame.frame != nullptr)
            output = complete_frame(decoding_frame);

        MutexLocker locker { m_mutex };
        if (decoding_frame.generation == m_generation) {
            if (!output.has_value())
                m_completed_end_of_stream = true;
            else if (output->is_error())
                m_decode_failure = output->release_error();
            else
                m_outputs.append(output->release_value());
            if (!m_caller_is_waiting_for_output && m_output_arrived_callback)
                m_output_arrived_callback();
        }
        m_condition.broadcast();
    }
}

DecoderErrorOr<FFmpegVulkanVideoDecoder::DecodedOutput> FFmpegVulkanVideoDecoder::complete_frame(DecodingFrame& decoding_frame)
{
    ScopeGuard free_frame = [&] { m_functions.av_frame_free(&decoding_frame.frame); };
    auto& frame = *decoding_frame.frame;
    auto logical_device = m_device.logical_device();

    if (!wait_for_semaphore(logical_device, decoding_frame.semaphore, decoding_frame.decoded_semaphore_value))
        return DecoderError::with_description(DecoderErrorCategory::Unknown, "The Vulkan device failed or stalled while decoding a frame"sv);

    // Mapping the frame to dma-bufs hands its image over to whichever device samples it next, which the frame then waits
    // for, so that it leaves the decoder ready to be drawn.
    auto* exported_frame = m_functions.av_frame_alloc();
    if (exported_frame == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg frame"sv);
    ScopeGuard free_exported_frame = [&] { m_functions.av_frame_free(&exported_frame); };
    exported_frame->format = AV_PIX_FMT_DRM_PRIME;
    {
        MutexLocker locker { m_frames_context_mutex };
        if (m_functions.av_hwframe_map(exported_frame, &frame, AV_HWFRAME_MAP_READ) < 0)
            return DecoderError::with_description(DecoderErrorCategory::Unknown, "Could not export a decoded frame as dma-bufs"sv);
    }
    auto exported = semaphore_value_of(frame);
    if (!wait_for_semaphore(logical_device, exported.semaphore, exported.value))
        return DecoderError::with_description(DecoderErrorCategory::Unknown, "The Vulkan device failed or stalled while readying a frame for export"sv);

    auto surface = TRY(surface_for_exported_frame(frame, *exported_frame));
    auto const& frames_context = frames_context_of(frame);
    Gfx::Size<u32> size { static_cast<u32>(frame.width), static_cast<u32>(frame.height) };
    auto bit_depth = bit_depth_of(frames_context);
    auto contents = DECODER_TRY_ALLOC(try_make_ref_counted<VulkanFrameContents>(m_functions, exchange(decoding_frame.frame, nullptr)));
    return DecodedOutput { move(surface), move(contents), decoding_frame.timing, size, bit_depth };
}

DecoderErrorOr<NonnullRefPtr<VideoSurface>> FFmpegVulkanVideoDecoder::surface_for_exported_frame(AVFrame const& frame, AVFrame const& exported_frame)
{
    auto const& frames_context = frames_context_of(frame);
    if (&frames_context != m_frames_context_of_surfaces) {
        m_surfaces_by_image.clear();
        m_frames_context_of_surfaces = &frames_context;
    }

    auto const& vulkan_frame = *reinterpret_cast<AVVkFrame const*>(frame.data[0]);
    u64 image_key = 0;
    static_assert(sizeof(vulkan_frame.img[0]) <= sizeof(image_key));
    memcpy(&image_key, &vulkan_frame.img[0], sizeof(vulkan_frame.img[0]));
    if (auto surface = m_surfaces_by_image.get(image_key); surface.has_value())
        return NonnullRefPtr { **surface };

    auto const& descriptor = *reinterpret_cast<AVDRMFrameDescriptor const*>(exported_frame.data[0]);
    if (descriptor.nb_objects < 1 || static_cast<size_t>(descriptor.nb_objects) > Gfx::LinuxDmaBufHandle::MAXIMUM_PLANE_COUNT)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "A decoded image exported to an unusable number of dma-bufs"sv);

    Gfx::LinuxDmaBufHandle dma_buf;
    dma_buf.size = { frames_context.width, frames_context.height };
    dma_buf.drm_format = frames_context.sw_format == AV_PIX_FMT_P010 ? DRM_FORMAT_P010 : DRM_FORMAT_NV12;
    dma_buf.modifier = descriptor.objects[0].format_modifier;
    for (int object_index = 0; object_index < descriptor.nb_objects; object_index++) {
        auto object = IPC::File::clone_fd(descriptor.objects[object_index].fd);
        if (object.is_error())
            return DecoderError::format(DecoderErrorCategory::Memory, "Could not duplicate a decoded image's dma-buf: {}", object.error());
        dma_buf.objects.append(object.release_value());
    }
    for (int layer_index = 0; layer_index < descriptor.nb_layers; layer_index++) {
        auto const& layer = descriptor.layers[layer_index];
        for (int plane_index = 0; plane_index < layer.nb_planes; plane_index++) {
            if (dma_buf.planes.size() == Gfx::LinuxDmaBufHandle::MAXIMUM_PLANE_COUNT)
                return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "A decoded image exported too many planes"sv);
            auto const& plane = layer.planes[plane_index];
            dma_buf.planes.append({ static_cast<u32>(plane.object_index), static_cast<u32>(plane.offset), static_cast<u32>(plane.pitch) });
        }
    }

    auto surface = VideoSurface::create_from_dma_buf(s_next_surface_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed), move(dma_buf));
    if (surface.is_error())
        return DecoderError::format(DecoderErrorCategory::Memory, "Could not create a surface for a decoded image: {}", surface.error());
    DECODER_TRY_ALLOC(m_surfaces_by_image.try_set(image_key, surface.value()));
    return surface.release_value();
}

DecoderErrorOr<NonnullRefPtr<VideoFrame>> FFmpegVulkanVideoDecoder::take_next_output(CodingIndependentCodePoints const& container_cicp, Optional<AK::Duration>)
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
    auto acquired_slot = m_surface_pool->try_acquire(output.surface, output.contents);
    if (!acquired_slot.has_value())
        return DecoderError::with_description(DecoderErrorCategory::TryAgain, "Every video frame slot is held elsewhere"sv);
    auto pool_slot = m_surface_pool->try_adopt_acquired_slot(*acquired_slot);
    if (pool_slot.is_error())
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate a pooled frame slot reference"sv);

    auto cicp = coding_independent_code_points_of(m_functions, *m_codec_context, container_cicp);
    auto frame = DECODER_TRY_ALLOC(try_make_ref_counted<VideoFrame>(output.timing.timestamp, output.timing.duration, output.size, output.bit_depth, Subsampling::yuv420(), cicp, pool_slot.release_value()));
    m_outputs.take_first();
    return frame;
}

void FFmpegVulkanVideoDecoder::flush()
{
    m_functions.avcodec_flush_buffers(m_codec_context);
    m_frames_awaiting_output.clear();
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

}
