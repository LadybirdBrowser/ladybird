/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Loader.h"
#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
}

namespace Audio {

class FFmpegIOContext {
public:
    explicit FFmpegIOContext(AVIOContext*);
    ~FFmpegIOContext();

    static ErrorOr<NonnullOwnPtr<FFmpegIOContext>, LoaderError> create(AK::SeekableStream& stream);

    AVIOContext* avio_context() const { return m_avio_context; }

private:
    AVIOContext* m_avio_context { nullptr };
};

class FFmpegLoaderPlugin : public LoaderPlugin {
public:
    explicit FFmpegLoaderPlugin(NonnullOwnPtr<SeekableStream>, NonnullOwnPtr<FFmpegIOContext>);
    virtual ~FFmpegLoaderPlugin();

    static bool sniff(SeekableStream& stream);
    static ErrorOr<NonnullOwnPtr<LoaderPlugin>, LoaderError> create(NonnullOwnPtr<SeekableStream>);

    virtual ErrorOr<Vector<FixedArray<Sample>>, LoaderError> load_chunks(size_t samples_to_read_from_input) override;

    virtual MaybeLoaderError reset() override;
    virtual MaybeLoaderError seek(int sample_index) override;

    virtual int loaded_samples() override { return m_loaded_samples; }
    virtual int total_samples() override { return m_total_samples; }
    virtual u32 sample_rate() override;
    virtual u16 num_channels() override;
    virtual PcmSampleFormat pcm_format() override;
    virtual ByteString format_name() override;

private:
    MaybeLoaderError initialize();
    double time_base() const;

    AVStream* m_audio_stream;
    AVCodecContext* m_codec_context { nullptr };
    AVFormatContext* m_format_context { nullptr };
    AVFrame* m_frame { nullptr };
    NonnullOwnPtr<FFmpegIOContext> m_io_context;
    int m_loaded_samples { 0 };
    AVPacket* m_packet { nullptr };
    int m_total_samples { 0 };
};

}
