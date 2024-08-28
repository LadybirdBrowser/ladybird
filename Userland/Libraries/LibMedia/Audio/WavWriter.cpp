/*
 * Copyright (c) 2020, William McPherson <willmcpherson2@gmail.com>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WavLoader.h"
#include "WavTypes.h"
#include "WavWriter.h"
#include <AK/Endian.h>

namespace Audio {

ErrorOr<NonnullOwnPtr<WavWriter>> WavWriter::create_from_file(StringView path, u32 sample_rate, u16 num_channels, PcmSampleFormat sample_format)
{
    auto wav_writer = TRY(adopt_nonnull_own_or_enomem(new (nothrow) WavWriter(sample_rate, num_channels, sample_format)));
    TRY(wav_writer->set_file(path));
    return wav_writer;
}

WavWriter::WavWriter(u32 sample_rate, u16 num_channels, PcmSampleFormat sample_format)
    : m_sample_rate(sample_rate)
    , m_num_channels(num_channels)
    , m_sample_format(sample_format)
{
}

WavWriter::~WavWriter()
{
    if (!m_finalized)
        (void)finalize();
}

ErrorOr<void> WavWriter::set_file(StringView path)
{
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write));
    m_file = TRY(Core::OutputBufferedFile::create(move(file)));
    TRY(m_file->seek(44, SeekMode::SetPosition));
    m_finalized = false;
    return {};
}

ErrorOr<void> WavWriter::write_samples(ReadonlySpan<Sample> samples)
{
    switch (m_sample_format) {
    // FIXME: For non-float formats, we don't add good quantization noise, leading to possibly unpleasant quantization artifacts.
    case PcmSampleFormat::Uint8: {
        constexpr float scale = static_cast<float>(NumericLimits<u8>::max()) * .5f;
        for (auto const& sample : samples) {
            u8 left = static_cast<u8>((sample.left + 1) * scale);
            u8 right = static_cast<u8>((sample.right + 1) * scale);
            TRY(m_file->write_value(left));
            if (m_num_channels >= 2)
                TRY(m_file->write_value(right));
        }
        m_data_sz += samples.size() * m_num_channels * sizeof(u8);
        break;
    }
    case PcmSampleFormat::Int16: {
        constexpr float scale = static_cast<float>(NumericLimits<i16>::max());
        for (auto const& sample : samples) {
            u16 left = AK::convert_between_host_and_little_endian(static_cast<i16>(sample.left * scale));
            u16 right = AK::convert_between_host_and_little_endian(static_cast<i16>(sample.right * scale));
            TRY(m_file->write_value(left));
            if (m_num_channels >= 2)
                TRY(m_file->write_value(right));
        }
        m_data_sz += samples.size() * m_num_channels * sizeof(u16);
        break;
    }
    default:
        VERIFY_NOT_REACHED();
    }

    return {};
}

ErrorOr<void> WavWriter::finalize()
{
    VERIFY(!m_finalized);
    m_finalized = true;

    if (m_file && m_file->is_open()) {
        TRY(m_file->seek(0, SeekMode::SetPosition));
        TRY(write_header());
        m_file->close();
    }
    m_data_sz = 0;
    return {};
}

ErrorOr<void> WavWriter::write_header()
{
    // "RIFF"
    static u32 riff = 0x46464952;
    TRY(m_file->write_value(riff));

    // Size of data + (size of header - previous field - this field)
    u32 sz = m_data_sz + (44 - 4 - 4);
    TRY(m_file->write_value(sz));

    // "WAVE"
    static u32 wave = 0x45564157;
    TRY(m_file->write_value(wave));

    // "fmt "
    static u32 fmt_id = 0x20746D66;
    TRY(m_file->write_value(fmt_id));

    // Size of the next 6 fields
    static u32 fmt_size = 16;
    TRY(m_file->write_value(fmt_size));

    static u16 audio_format = to_underlying(Wav::WaveFormat::Pcm);
    TRY(m_file->write_value(audio_format));

    TRY(m_file->write_value(m_num_channels));

    TRY(m_file->write_value(m_sample_rate));

    VERIFY(m_sample_format == PcmSampleFormat::Int16 || m_sample_format == PcmSampleFormat::Uint8);
    u16 bits_per_sample = pcm_bits_per_sample(m_sample_format);
    u32 byte_rate = m_sample_rate * m_num_channels * (bits_per_sample / 8);
    TRY(m_file->write_value(byte_rate));

    u16 block_align = m_num_channels * (bits_per_sample / 8);
    TRY(m_file->write_value(block_align));

    TRY(m_file->write_value(bits_per_sample));

    // "data"
    static u32 chunk_id = 0x61746164;
    TRY(m_file->write_value(chunk_id));

    TRY(m_file->write_value(m_data_sz));

    return {};
}

}
