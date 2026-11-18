/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibGfx/VulkanContext.h>
#include <LibMedia/DemuxerRegistry.h>
#include <LibMediaCodecs/FFmpeg/FFmpegVulkanVideoDecoder.h>
#include <LibMediaCodecs/FFmpeg/SystemFFmpeg.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/Vulkan/VulkanVideoDevice.h>
#include <LibPlatformFeatures/ProcessFeatures.h>

#include "TestMediaCommon.h"

namespace {

bool create_process_device()
{
    if (Media::VulkanVideoDevice::the() != nullptr)
        return true;

    // Each suite runs in its own process, so this sets up the process's features once, as VideoDecoderServer would
    // with nothing blocked.
    static auto initialized_process = [] {
        PlatformFeatures::ProcessFeatures::initialize_for_current_process(MUST(PlatformFeatures::ProcessFeatures::from_string(PlatformFeatures::FeatureProcess::VideoDecoderServer,
            "vulkan_video,vulkan_video.bundled_ffmpeg,vulkan_video.bundled_ffmpeg.direct_export,vulkan_video.bundled_ffmpeg.vp9,vulkan_video.bundled_ffmpeg.av1,"
            "vulkan_video.system_ffmpeg,vulkan_video.system_ffmpeg.direct_export,vulkan_video.system_ffmpeg.h264,vulkan_video.system_ffmpeg.h265"sv)));
        Media::FFmpeg::SystemFFmpeg::load();
        return true;
    }();
    (void)initialized_process;

    VkApplicationInfo application_info {};
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info {};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application_info;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS)
        return false;
    ScopeGuard destroy_instance = [&] { vkDestroyInstance(instance, nullptr); };

    u32 count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    Vector<VkPhysicalDevice> physical_devices;
    physical_devices.resize(count);
    vkEnumeratePhysicalDevices(instance, &count, physical_devices.data());
    for (auto* physical_device : physical_devices) {
        if (!Media::VulkanVideoDevice::create_for_process(Gfx::gpu_device_identity_of(physical_device)).is_error())
            return true;
    }
    return false;
}

struct DecodedFrame {
    AK::Duration timestamp;
    Gfx::Size<u32> size;
};

struct Decoding {
    NonnullOwnPtr<Media::FFmpeg::FFmpegVulkanVideoDecoder> decoder;
    NonnullRefPtr<Media::Demuxer> demuxer;
    Media::Track track;
    mutable size_t accepted_sample_count { 1 };

    Media::DecoderErrorOr<NonnullRefPtr<Media::VideoFrame>> next_frame() const
    {
        while (true) {
            auto frame_result = decoder->take_next_output(track.video_data().color_metadata);
            if (!frame_result.is_error() || frame_result.error().category() != Media::DecoderErrorCategory::NeedsMoreInput)
                return frame_result;

            auto sample_result = demuxer->get_next_sample_for_track(track);
            if (sample_result.is_error()) {
                decoder->signal_end_of_stream();
                continue;
            }
            TRY(decoder->receive_coded_data(sample_result.release_value(), Media::DecodeIntent::Output));
            accepted_sample_count++;
        }
    }

    Media::DecoderError decode_all(Vector<DecodedFrame>& frames) const
    {
        while (true) {
            auto frame_result = next_frame();
            if (frame_result.is_error())
                return frame_result.release_error();
            auto frame = frame_result.release_value();
            frames.append({ frame->timestamp(), frame->size() });
        }
    }
};

Optional<Decoding> open_decoding(StringView path, Media::CodecID codec_id)
{
    if (!create_process_device()) {
        warnln("No Vulkan device can decode video, skipping");
        return {};
    }

    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video));
    VERIFY(track.has_value());
    MUST(demuxer->create_context_for_track(*track));

    auto first_sample = MUST(demuxer->get_next_sample_for_track(*track));
    auto start_decoding = [&] -> Media::DecoderErrorOr<NonnullOwnPtr<Media::FFmpeg::FFmpegVulkanVideoDecoder>> {
        auto decoder = TRY(Media::FFmpeg::FFmpegVulkanVideoDecoder::try_create(codec_id, first_sample.new_codec_configuration().value_or({})));
        // Whether the device can decode and export the stream is only known once the first frame negotiates its format.
        TRY(decoder->receive_coded_data(first_sample, Media::DecodeIntent::Output));
        return decoder;
    };
    auto decoder_result = start_decoding();
    if (decoder_result.is_error()) {
        if (decoder_result.error().category() == Media::DecoderErrorCategory::NotImplemented) {
            warnln("No Vulkan decoder for codec {}, skipping: {}", codec_id, decoder_result.error().description());
            return {};
        }
        FAIL(ByteString::formatted("Could not start Vulkan decoding of codec {}: {}", codec_id, decoder_result.error().description()));
        return {};
    }
    return Decoding { decoder_result.release_value(), move(demuxer), track.release_value() };
}

void expect_every_frame_across_a_size_change(StringView path, Media::CodecID codec_id, size_t frame_count, Gfx::Size<u32> first_size, Gfx::Size<u32> second_size)
{
    auto decoding = open_decoding(path, codec_id);
    if (!decoding.has_value())
        return;

    Vector<DecodedFrame> frames;
    auto error = decoding->decode_all(frames);
    EXPECT_EQ(error.category(), Media::DecoderErrorCategory::EndOfStream);
    EXPECT_EQ(frames.size(), frame_count);

    size_t size_changes = 0;
    for (size_t i = 1; i < frames.size(); i++) {
        EXPECT(frames[i - 1].timestamp < frames[i].timestamp);
        if (frames[i - 1].size != frames[i].size)
            size_changes++;
    }
    EXPECT_EQ(size_changes, 1u);
    if (!frames.is_empty()) {
        EXPECT_EQ(frames.first().size, first_size);
        EXPECT_EQ(frames.last().size, second_size);
    }
}

}

TEST_CASE(vp9_keyframe_size_change_continues_in_a_new_codec_context)
{
    expect_every_frame_across_a_size_change("./vp9_resize.webm"sv, Media::CodecID::VP9, 2, { 320, 240 }, { 640, 480 });
}

TEST_CASE(h264_size_change_keeps_reordered_frames)
{
    expect_every_frame_across_a_size_change("./h264_keyframe_resize.mkv"sv, Media::CodecID::H264, 20, { 320, 240 }, { 160, 120 });
}

TEST_CASE(h265_size_change_keeps_reordered_frames)
{
    expect_every_frame_across_a_size_change("./h265_keyframe_resize.mkv"sv, Media::CodecID::H265, 20, { 320, 240 }, { 256, 160 });
}

TEST_CASE(vp9_inter_frame_size_change_needs_another_decoder)
{
    auto decoding = open_decoding("./vp9_inter_frame_resize.webm"sv, Media::CodecID::VP9);
    if (!decoding.has_value())
        return;

    Vector<DecodedFrame> frames;
    auto error = decoding->decode_all(frames);
    EXPECT_EQ(error.category(), Media::DecoderErrorCategory::NotImplemented);
    // The keyframe decodes, and the inter-frame after it changes the size.
    EXPECT_EQ(decoding->accepted_sample_count, 1u);
    for (auto const& frame : frames)
        EXPECT_EQ(frame.size, Gfx::Size<u32>(1280, 720));
}

// The file's mastering display and content light levels are only in metadata OBUs, which must survive the copy of a frame
// made for export.
TEST_CASE(frames_carry_the_high_dynamic_range_metadata_in_their_bitstream)
{
    auto decoding = open_decoding("./av1_hdr_metadata_in_webm.webm"sv, Media::CodecID::AV1);
    if (!decoding.has_value())
        return;

    auto frame = MUST(decoding->next_frame());
    auto const& color_metadata = frame->color_metadata();
    EXPECT(color_metadata.content_light_level_information.has_value());
    if (color_metadata.content_light_level_information.has_value()) {
        EXPECT_EQ(color_metadata.content_light_level_information->maximum_content_light_level, 1005);
        EXPECT_EQ(color_metadata.content_light_level_information->maximum_frame_average_light_level, 315);
    }
    EXPECT(color_metadata.mastering_display_color_volume.has_value());
    if (color_metadata.mastering_display_color_volume.has_value())
        EXPECT_APPROXIMATE(color_metadata.mastering_display_color_volume->maximum_luminance, 1000.0);
}

static void expect_scene_peaks_of_every_frame(StringView path, Span<ExpectedScenePeak const> expected_scene_peaks)
{
    ScenePeakChecker scene_peaks { expected_scene_peaks };
    auto decoding = open_decoding(path, Media::CodecID::H265);
    if (!decoding.has_value())
        return;

    size_t frame_count = 0;
    while (true) {
        auto frame_result = decoding->next_frame();
        if (frame_result.is_error()) {
            EXPECT_EQ(frame_result.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        auto const& frame = *frame_result.value();
        scene_peaks.check(frame.timestamp(), frame.color_metadata().maximum_scene_luminance);
        frame_count++;
    }
    EXPECT_EQ(frame_count, expected_scene_peaks.size());
    scene_peaks.expect_every_frame_checked();
}

TEST_CASE(h265_frames_carry_their_own_hdr10_plus_scene_peaks)
{
    expect_scene_peaks_of_every_frame("./hevc_hdr10_plus_in_sei.mkv"sv, hevc_hdr10_plus_in_sei_scene_peaks);
}

TEST_CASE(h265_frames_carry_their_own_dolby_vision_scene_peaks)
{
    expect_scene_peaks_of_every_frame("./hevc_dolby_vision_rpus.mkv"sv, hevc_dolby_vision_rpus_scene_peaks);
}
