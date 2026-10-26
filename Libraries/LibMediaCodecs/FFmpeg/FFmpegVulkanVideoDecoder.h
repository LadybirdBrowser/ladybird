/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef USE_VULKAN_DMABUF_IMAGES

#    include <AK/ConditionVariable.h>
#    include <AK/HashMap.h>
#    include <AK/Mutex.h>
#    include <AK/Queue.h>
#    include <LibMediaCodecs/CodecID.h>
#    include <LibMediaCodecs/CodecParameters.h>
#    include <LibMediaCodecs/DecoderCapabilities.h>
#    include <LibMediaCodecs/Export.h>
#    include <LibMediaCodecs/FFmpeg/FFmpegFunctions.h>
#    include <LibMediaCodecs/FFmpeg/VideoFramesAwaitingOutput.h>
#    include <LibMediaCodecs/VideoDecoder.h>
#    include <LibMediaCodecs/VideoFramePool.h>
#    include <LibMediaCodecs/VideoSurface.h>
#    include <LibMediaCodecs/Vulkan/VulkanVideoDevice.h>
#    include <LibThreading/Forward.h>

namespace Media::FFmpeg {

// Decodes through FFmpeg's Vulkan hardware acceleration into images that export as dma-bufs. The device decodes
// asynchronously, so a thread of the decoder's own waits for each frame to finish, readies it for another device to
// sample, and hands it out.
class MEDIACODECS_API FFmpegVulkanVideoDecoder final : public VideoDecoder {
public:
    AK_ALLOC_WITH_KMALLOC;

    static Optional<DecoderCapabilities> capabilities(ParsedCodec const&);
    static DecoderErrorOr<NonnullOwnPtr<FFmpegVulkanVideoDecoder>> try_create(CodecID, ReadonlyBytes codec_initialization_data);
    virtual ~FFmpegVulkanVideoDecoder() override;

    virtual void set_storage_freed_callback(Function<void()> callback) override { m_surface_pool->set_slot_freed_callback(move(callback)); }
    virtual void set_output_arrived_callback(Function<void()> callback) override
    {
        MutexLocker locker { m_mutex };
        m_output_arrived_callback = move(callback);
    }

    virtual DecoderErrorOr<void> receive_coded_data(CodedFrame const&, DecodeIntent) override;
    virtual void signal_end_of_stream() override;
    virtual DecoderErrorOr<NonnullRefPtr<VideoFrame>> take_next_output(CodingIndependentCodePoints const& container_cicp, Optional<AK::Duration> target = {}) override;
    virtual void flush() override;

private:
    struct DecodingFrame {
        // None marks the end of the stream, after the last frame.
        AVFrame* frame { nullptr };
        VideoFramesAwaitingOutput::Frame timing;
        u64 generation { 0 };
        // Later decodes that reference the frame advance its semaphore further, so the value its own decode signals
        // is taken as soon as FFmpeg hands the frame out.
        VkSemaphore semaphore { VK_NULL_HANDLE };
        u64 decoded_semaphore_value { 0 };
    };

    struct DecodedOutput {
        NonnullRefPtr<VideoSurface> surface;
        // Holds the decoded image out of FFmpeg's pool until the slot the frame goes out in is released.
        NonnullRefPtr<VideoSurfaceContents> contents;
        VideoFramesAwaitingOutput::Frame timing;
        Gfx::Size<u32> size;
        u8 bit_depth { 0 };
    };

    FFmpegVulkanVideoDecoder(FFmpegFunctions const&, VulkanVideoDevice&, AVCodecContext*, AVPacket*, AVFrame*, NonnullRefPtr<VideoFrameSurfacePool>);

    static AVPixelFormat negotiate_pixel_format(AVCodecContext*, AVPixelFormat const*);
    static int get_buffer(AVCodecContext*, AVFrame*, int flags);
    DecoderErrorOr<void> create_frames_context();

    DecoderErrorOr<void> receive_decoded_frames();
    void enqueue_decoding_frame(DecodingFrame);
    void free_decoding_frames(Vector<DecodingFrame>&);

    void complete_frames_until_exit();
    DecoderErrorOr<DecodedOutput> complete_frame(DecodingFrame&);
    DecoderErrorOr<NonnullRefPtr<VideoSurface>> surface_for_exported_frame(AVFrame const& frame, AVFrame const& exported_frame);

    FFmpegFunctions const& m_functions;
    VulkanVideoDevice& m_device;
    AVCodecContext* m_codec_context { nullptr };
    AVPacket* m_packet { nullptr };
    AVFrame* m_received_frame { nullptr };
    NonnullRefPtr<VideoFrameSurfacePool> m_surface_pool;

    // Only the caller's thread uses these.
    VideoFramesAwaitingOutput m_frames_awaiting_output;
    Optional<DecoderError> m_pixel_format_error;
    bool m_received_end_of_stream { false };
    Vector<u64> m_drm_format_modifiers;
    VkImageDrmFormatModifierListCreateInfoEXT m_drm_format_modifier_list {};

    // FFmpeg records the work that allocates images and readies them for export on a frames context's own command
    // buffers, which it does not keep threads from sharing.
    Mutex m_frames_context_mutex;

    mutable Mutex m_mutex;
    ConditionVariable m_condition { m_mutex };
    Queue<DecodingFrame> m_decoding_frames;
    Vector<DecodedOutput> m_outputs;
    Optional<DecoderError> m_decode_failure;
    bool m_completed_end_of_stream { false };
    bool m_caller_is_waiting_for_output { false };
    bool m_completion_thread_should_exit { false };
    u64 m_generation { 0 };
    Function<void()> m_output_arrived_callback;
    RefPtr<Threading::Thread> m_completion_thread;

    // Only the completion thread uses these. A frames context recycles its images, so each one's surface is kept and
    // recognized when it comes back.
    AVHWFramesContext const* m_frames_context_of_surfaces { nullptr };
    HashMap<u64, NonnullRefPtr<VideoSurface>> m_surfaces_by_image;
};

}

#endif
