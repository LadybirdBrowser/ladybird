/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Vector.h>
#include <LibMediaCodecs/Vulkan/VulkanVideoDevice.h>
#include <SystemProbe/VulkanVideoDescription.h>

struct VideoDecodeQuery {
    VkPhysicalDevice physical_device { VK_NULL_HANDLE };
    Vector<VkExtensionProperties> extensions;
    PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR get_capabilities { nullptr };
    PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR get_formats { nullptr };
};

// Asks about the codec's 8-bit 4:2:0 profile, which every stream the decoders take in that codec can be decoded under.
static PlatformFeatures::VideoDecodeFacts describe_video_decoding(VideoDecodeQuery const& query, StringView extension, VkVideoCodecOperationFlagBitsKHR operation, void* codec_profile, void* codec_capabilities)
{
    PlatformFeatures::VideoDecodeFacts facts;
    auto has_extension = any_of(query.extensions, [&](auto const& properties) { return StringView { properties.extensionName, strlen(properties.extensionName) } == extension; });
    if (!has_extension || query.get_capabilities == nullptr || query.get_formats == nullptr)
        return facts;

    VkVideoProfileInfoKHR profile {};
    profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    profile.pNext = codec_profile;
    profile.videoCodecOperation = operation;
    profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

    VkVideoDecodeCapabilitiesKHR decode_capabilities {};
    decode_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
    decode_capabilities.pNext = codec_capabilities;
    VkVideoCapabilitiesKHR capabilities {};
    capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
    capabilities.pNext = &decode_capabilities;
    if (query.get_capabilities(query.physical_device, &profile, &capabilities) != VK_SUCCESS)
        return facts;

    facts.supported = true;
    facts.dpb_and_output_coincide = (decode_capabilities.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) != 0;
    facts.dpb_and_output_distinct = (decode_capabilities.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR) != 0;

    VkVideoProfileListInfoKHR profile_list {};
    profile_list.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    profile_list.profileCount = 1;
    profile_list.pProfiles = &profile;
    VkPhysicalDeviceVideoFormatInfoKHR format_info {};
    format_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR;
    format_info.pNext = &profile_list;
    format_info.imageUsage = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
    u32 format_count = 0;
    if (query.get_formats(query.physical_device, &format_info, &format_count, nullptr) != VK_SUCCESS)
        return facts;
    Vector<VkVideoFormatPropertiesKHR> formats;
    formats.resize(format_count);
    for (auto& format : formats)
        format.sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
    if (query.get_formats(query.physical_device, &format_info, &format_count, formats.data()) != VK_SUCCESS)
        return facts;
    facts.dpb_supports_drm_format_modifiers = any_of(formats, [](auto const& format) { return format.imageTiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT; });
    return facts;
}

void describe_video_decoding_through_vulkan(PlatformFeatures::GpuFacts& gpu, VkInstance instance, VkPhysicalDevice physical_device)
{
    if (auto result = Media::VulkanVideoDevice::check_can_decode_video(physical_device); result.is_error()) {
        gpu.cannot_decode_video_because = MUST(String::from_utf8(result.error().string_literal()));
        return;
    }

    gpu.supports_video_maintenance2 = Media::VulkanVideoDevice::supports_video_maintenance2(physical_device);

    VideoDecodeQuery query;
    query.physical_device = physical_device;
    u32 extension_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, nullptr);
    query.extensions.resize(extension_count);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, query.extensions.data());
    query.get_capabilities = reinterpret_cast<PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceVideoCapabilitiesKHR"));
    query.get_formats = reinterpret_cast<PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceVideoFormatPropertiesKHR"));

    VkVideoDecodeH264ProfileInfoKHR h264_profile {};
    h264_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
    h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
    h264_profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
    VkVideoDecodeH264CapabilitiesKHR h264_capabilities {};
    h264_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR;
    gpu.h264 = describe_video_decoding(query, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME ""sv, VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, &h264_profile, &h264_capabilities);

    VkVideoDecodeH265ProfileInfoKHR h265_profile {};
    h265_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
    h265_profile.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
    VkVideoDecodeH265CapabilitiesKHR h265_capabilities {};
    h265_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR;
    gpu.h265 = describe_video_decoding(query, VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME ""sv, VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR, &h265_profile, &h265_capabilities);

    VkVideoDecodeVP9ProfileInfoKHR vp9_profile {};
    vp9_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PROFILE_INFO_KHR;
    vp9_profile.stdProfile = STD_VIDEO_VP9_PROFILE_0;
    VkVideoDecodeVP9CapabilitiesKHR vp9_capabilities {};
    vp9_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_CAPABILITIES_KHR;
    gpu.vp9 = describe_video_decoding(query, VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME ""sv, VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR, &vp9_profile, &vp9_capabilities);

    VkVideoDecodeAV1ProfileInfoKHR av1_profile {};
    av1_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR;
    av1_profile.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;
    VkVideoDecodeAV1CapabilitiesKHR av1_capabilities {};
    av1_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR;
    gpu.av1 = describe_video_decoding(query, VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME ""sv, VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR, &av1_profile, &av1_capabilities);
}
