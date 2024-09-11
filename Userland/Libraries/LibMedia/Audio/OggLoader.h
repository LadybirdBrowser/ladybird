/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Loader.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace Audio {

class OggLoaderPlugin : public LoaderPlugin {
public:
    explicit OggLoaderPlugin(NonnullOwnPtr<SeekableStream> stream);
    virtual ~OggLoaderPlugin();

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
    virtual ByteString format_name() override { return "Ogg Vorbis (.ogg)"; }

private:
    MaybeLoaderError initialize();
    double time_base() const;

    void* m_avio_buffer { nullptr };
    AVIOContext* m_avio_context { nullptr };
    AVCodecContext* m_codec_context { nullptr };
    AVFormatContext* m_format_context { nullptr };
    AVStream* m_audio_stream;
    AVFrame* m_frame;
    AVPacket* m_packet;

    int m_loaded_samples { 0 };
    int m_total_samples { 0 };
};

}
