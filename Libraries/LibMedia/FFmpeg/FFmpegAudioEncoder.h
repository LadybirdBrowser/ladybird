/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Time.h>
#include <LibMedia/Export.h>
#include <LibMediaCodecs/AudioBlock.h>
#include <LibMediaCodecs/CodecID.h>
#include <LibMediaCodecs/DecoderError.h>
#include <LibMediaCodecs/FFmpeg/FFmpegForward.h>

namespace Media::FFmpeg {

// Feed PCM via receive_pcm_data, then drain encoded packets via write_next_packet until it reports NeedsMoreInput.
class MEDIA_API FFmpegAudioEncoder final {
    AK_ALLOC_WITH_KMALLOC;

public:
    struct Packet {
        ByteBuffer data;
        AK::Duration timestamp;
        AK::Duration duration;
    };

    static DecoderErrorOr<NonnullOwnPtr<FFmpegAudioEncoder>> try_create(CodecID, Audio::SampleSpecification const&, int bitrate);
    FFmpegAudioEncoder(FFmpegFunctions const&, AVCodecContext*, AVPacket*, AVFrame*);
    ~FFmpegAudioEncoder();

    int frame_size() const;

    DecoderErrorOr<void> receive_pcm_data(AK::Duration timestamp, AudioBlock const& block);
    DecoderErrorOr<bool> write_next_packet(Packet& out);
    void signal_end_of_stream();
    void flush();

private:
    FFmpegFunctions const& m_functions;
    AVCodecContext* m_codec_context;
    AVPacket* m_packet;
    AVFrame* m_frame;
};

}
