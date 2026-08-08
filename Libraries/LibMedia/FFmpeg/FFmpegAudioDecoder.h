/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/AudioDecoder.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/CodecParameters.h>
#include <LibMedia/DecoderCapabilities.h>
#include <LibMedia/Export.h>
#include <LibMedia/FFmpeg/FFmpegForward.h>

namespace Media::FFmpeg {

class MEDIA_API FFmpegAudioDecoder final : public AudioDecoder {
public:
    static Optional<DecoderCapabilities> capabilities(ParsedCodec const&);
    static DecoderErrorOr<NonnullOwnPtr<FFmpegAudioDecoder>> try_create(CodecID, Audio::SampleSpecification const&, ReadonlyBytes codec_initialization_data);
    FFmpegAudioDecoder(AVCodecContext* codec_context, AVPacket* packet, AVFrame* frame);
    virtual ~FFmpegAudioDecoder() override;

    virtual DecoderErrorOr<void> receive_coded_data(CodedFrame const&) override;
    virtual void signal_end_of_stream() override;
    // Writes buffered audio samples to the provided block, up to its capacity.
    virtual DecoderErrorOr<void> write_next_block(AudioBlock&) override;

    virtual void flush() override;

private:
    AVCodecContext* m_codec_context;
    AVPacket* m_packet;
    AVFrame* m_frame;
    size_t m_frame_read_offset { 0 };
};

}
