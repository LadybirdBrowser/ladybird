/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/AudioDecoder.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/Export.h>
#include <LibMedia/FFmpeg/FFmpegForward.h>

namespace Media::FFmpeg {

class MEDIA_API FFmpegAudioDecoder final : public AudioDecoder {
public:
    static DecoderErrorOr<NonnullOwnPtr<FFmpegAudioDecoder>> try_create(CodecID, ReadonlyBytes codec_initialization_data);
    FFmpegAudioDecoder(AVCodecContext* codec_context, AVPacket* packet, AVFrame* frame);
    virtual ~FFmpegAudioDecoder() override;

    virtual DecoderErrorOr<void> receive_coded_data(AK::Duration timestamp, ReadonlyBytes coded_data) override;
    virtual void signal_end_of_stream() override;
    // Writes all buffered audio samples to the provided block.
    virtual DecoderErrorOr<void> write_next_block(AudioBlock&) override;

    virtual void flush() override;

private:
    AVCodecContext* m_codec_context;
    AVPacket* m_packet;
    AVFrame* m_frame;
};

}
