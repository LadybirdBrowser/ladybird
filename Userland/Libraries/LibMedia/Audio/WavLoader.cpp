/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WavLoader.h"
#include "LoaderError.h"
#include "WavTypes.h"
#include <AK/Debug.h>
#include <AK/Endian.h>
#include <AK/FixedArray.h>
#include <AK/MemoryStream.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NumericLimits.h>
#include <AK/Try.h>

namespace Audio {

WavLoaderPlugin::WavLoaderPlugin(NonnullOwnPtr<SeekableStream> stream)
    : LoaderPlugin(move(stream))
{
}

bool WavLoaderPlugin::sniff(SeekableStream& stream)
{
    auto riff = stream.read_value<RIFF::ChunkID>();
    if (riff.is_error())
        return false;
    if (riff.value() != RIFF::riff_magic)
        return false;

    auto size = stream.read_value<LittleEndian<u32>>();
    if (size.is_error())
        return false;

    auto wave = stream.read_value<RIFF::ChunkID>();
    return !wave.is_error() && wave.value() == Wav::wave_subformat_id;
}

ErrorOr<NonnullOwnPtr<LoaderPlugin>, LoaderError> WavLoaderPlugin::create(NonnullOwnPtr<SeekableStream> stream)
{
    auto loader = make<WavLoaderPlugin>(move(stream));
    TRY(loader->parse_header());
    return loader;
}

template<typename SampleReader>
MaybeLoaderError WavLoaderPlugin::read_samples_from_stream(Stream& stream, SampleReader read_sample, FixedArray<Sample>& samples) const
{
    switch (m_num_channels) {
    case 1:
        for (auto& sample : samples)
            sample = Sample(TRY(read_sample(stream)));
        break;
    case 2:
        for (auto& sample : samples) {
            auto left_channel_sample = TRY(read_sample(stream));
            auto right_channel_sample = TRY(read_sample(stream));
            sample = Sample(left_channel_sample, right_channel_sample);
        }
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    return {};
}

// There's no i24 type + we need to do the endianness conversion manually anyways.
static ErrorOr<double> read_sample_int24(Stream& stream)
{
    i32 sample1 = TRY(stream.read_value<u8>());
    i32 sample2 = TRY(stream.read_value<u8>());
    i32 sample3 = TRY(stream.read_value<u8>());

    i32 value = 0;
    value = sample1;
    value |= sample2 << 8;
    value |= sample3 << 16;
    // Sign extend the value, as it can currently not have the correct sign.
    value = (value << 8) >> 8;
    // Range of value is now -2^23 to 2^23-1 and we can rescale normally.
    return static_cast<double>(value) / static_cast<double>((1 << 23) - 1);
}

template<typename T>
static ErrorOr<double> read_sample(Stream& stream)
{
    T sample { 0 };
    TRY(stream.read_until_filled(Bytes { &sample, sizeof(T) }));
    // Remap integer samples to normalized floating-point range of -1 to 1.
    if constexpr (IsIntegral<T>) {
        if constexpr (NumericLimits<T>::is_signed()) {
            // Signed integer samples are centered around zero, so this division is enough.
            return static_cast<double>(AK::convert_between_host_and_little_endian(sample)) / static_cast<double>(NumericLimits<T>::max());
        } else {
            // Unsigned integer samples, on the other hand, need to be shifted to center them around zero.
            // The first division therefore remaps to the range 0 to 2.
            return static_cast<double>(AK::convert_between_host_and_little_endian(sample)) / (static_cast<double>(NumericLimits<T>::max()) / 2.0) - 1.0;
        }
    } else {
        return static_cast<double>(AK::convert_between_host_and_little_endian(sample));
    }
}

LoaderSamples WavLoaderPlugin::samples_from_pcm_data(ReadonlyBytes data, size_t samples_to_read) const
{
    FixedArray<Sample> samples = TRY(FixedArray<Sample>::create(samples_to_read));
    FixedMemoryStream stream { data };

    switch (m_sample_format) {
    case PcmSampleFormat::Uint8:
        TRY(read_samples_from_stream(stream, read_sample<u8>, samples));
        break;
    case PcmSampleFormat::Int16:
        TRY(read_samples_from_stream(stream, read_sample<i16>, samples));
        break;
    case PcmSampleFormat::Int24:
        TRY(read_samples_from_stream(stream, read_sample_int24, samples));
        break;
    case PcmSampleFormat::Float32:
        TRY(read_samples_from_stream(stream, read_sample<float>, samples));
        break;
    case PcmSampleFormat::Float64:
        TRY(read_samples_from_stream(stream, read_sample<double>, samples));
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    return samples;
}

ErrorOr<Vector<FixedArray<Sample>>, LoaderError> WavLoaderPlugin::load_chunks(size_t samples_to_read_from_input)
{
    auto remaining_samples = m_total_samples - m_loaded_samples;
    if (remaining_samples <= 0)
        return Vector<FixedArray<Sample>> {};

    // One "sample" contains data from all channels.
    // In the Wave spec, this is also called a block.
    size_t bytes_per_sample
        = m_num_channels * pcm_bits_per_sample(m_sample_format) / 8;

    auto samples_to_read = min(samples_to_read_from_input, remaining_samples);
    auto bytes_to_read = samples_to_read * bytes_per_sample;

    dbgln_if(AWAVLOADER_DEBUG, "Read {} bytes WAV with num_channels {} sample rate {}, "
                               "bits per sample {}, sample format {}",
        bytes_to_read, m_num_channels, m_sample_rate,
        pcm_bits_per_sample(m_sample_format), sample_format_name(m_sample_format));

    auto sample_data = TRY(ByteBuffer::create_zeroed(bytes_to_read));
    TRY(m_stream->read_until_filled(sample_data.bytes()));

    // m_loaded_samples should contain the amount of actually loaded samples
    m_loaded_samples += samples_to_read;
    Vector<FixedArray<Sample>> samples;
    TRY(samples.try_append(TRY(samples_from_pcm_data(sample_data.bytes(), samples_to_read))));
    return samples;
}

MaybeLoaderError WavLoaderPlugin::seek(int sample_index)
{
    dbgln_if(AWAVLOADER_DEBUG, "seek sample_index {}", sample_index);
    if (sample_index < 0 || sample_index >= static_cast<int>(m_total_samples))
        return LoaderError { LoaderError::Category::Internal, m_loaded_samples, "Seek outside the sample range" };

    size_t sample_offset = m_byte_offset_of_data_samples + static_cast<size_t>(sample_index * m_num_channels * (pcm_bits_per_sample(m_sample_format) / 8));

    TRY(m_stream->seek(sample_offset, SeekMode::SetPosition));

    m_loaded_samples = sample_index;
    return {};
}

// Specification reference: http://www-mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/WAVE.html
MaybeLoaderError WavLoaderPlugin::parse_header()
{
#define CHECK(check, category, msg)                                                                                                    \
    do {                                                                                                                               \
        if (!(check)) {                                                                                                                \
            return LoaderError { category, static_cast<size_t>(TRY(m_stream->tell())), ByteString::formatted("WAV header: {}", msg) }; \
        }                                                                                                                              \
    } while (0)

    auto file_header = TRY(m_stream->read_value<RIFF::FileHeader>());
    CHECK(file_header.magic() == RIFF::riff_magic, LoaderError::Category::Format, "RIFF header magic invalid");
    CHECK(file_header.subformat == Wav::wave_subformat_id, LoaderError::Category::Format, "WAVE subformat id invalid");

    auto format_chunk = TRY(m_stream->read_value<RIFF::OwnedChunk>());
    CHECK(format_chunk.id().as_ascii_string() == Wav::format_chunk_id, LoaderError::Category::Format, "FMT chunk id invalid");

    auto format_stream = format_chunk.data_stream();
    u16 audio_format = TRY(format_stream.read_value<LittleEndian<u16>>());
    CHECK(audio_format == to_underlying(Wav::WaveFormat::Pcm) || audio_format == to_underlying(Wav::WaveFormat::IEEEFloat) || audio_format == to_underlying(Wav::WaveFormat::Extensible),
        LoaderError::Category::Unimplemented, "Audio format not supported");

    m_num_channels = TRY(format_stream.read_value<LittleEndian<u16>>());
    CHECK(m_num_channels == 1 || m_num_channels == 2, LoaderError::Category::Unimplemented, "Channel count");

    m_sample_rate = TRY(format_stream.read_value<LittleEndian<u32>>());
    // Data rate; can be ignored.
    TRY(format_stream.read_value<LittleEndian<u32>>());
    u16 block_size_bytes = TRY(format_stream.read_value<LittleEndian<u16>>());

    u16 bits_per_sample = TRY(format_stream.read_value<LittleEndian<u16>>());

    if (audio_format == to_underlying(Wav::WaveFormat::Extensible)) {
        CHECK(format_chunk.size() == 40, LoaderError::Category::Format, "Extensible fmt size is not 40 bytes");

        // Discard everything until the GUID.
        // We've already read 16 bytes from the stream. The GUID starts in another 8 bytes.
        TRY(format_stream.read_value<LittleEndian<u64>>());

        // Get the underlying audio format from the first two bytes of GUID
        u16 guid_subformat = TRY(format_stream.read_value<LittleEndian<u16>>());
        CHECK(guid_subformat == to_underlying(Wav::WaveFormat::Pcm) || guid_subformat == to_underlying(Wav::WaveFormat::IEEEFloat), LoaderError::Category::Unimplemented, "GUID SubFormat not supported");

        audio_format = guid_subformat;
    }

    if (audio_format == to_underlying(Wav::WaveFormat::Pcm)) {
        CHECK(bits_per_sample == 8 || bits_per_sample == 16 || bits_per_sample == 24, LoaderError::Category::Unimplemented, "PCM bits per sample not supported");

        // We only support 8-24 bit audio right now because other formats are uncommon
        if (bits_per_sample == 8) {
            m_sample_format = PcmSampleFormat::Uint8;
        } else if (bits_per_sample == 16) {
            m_sample_format = PcmSampleFormat::Int16;
        } else if (bits_per_sample == 24) {
            m_sample_format = PcmSampleFormat::Int24;
        }
    } else if (audio_format == to_underlying(Wav::WaveFormat::IEEEFloat)) {
        CHECK(bits_per_sample == 32 || bits_per_sample == 64, LoaderError::Category::Unimplemented, "Float bits per sample not supported");

        // Again, only the common 32 and 64 bit
        if (bits_per_sample == 32) {
            m_sample_format = PcmSampleFormat::Float32;
        } else if (bits_per_sample == 64) {
            m_sample_format = PcmSampleFormat::Float64;
        }
    }

    CHECK(block_size_bytes == (m_num_channels * (bits_per_sample / 8)), LoaderError::Category::Format, "Block size invalid");

    dbgln_if(AWAVLOADER_DEBUG, "WAV format {} at {} bit, {} channels, rate {}Hz ",
        sample_format_name(m_sample_format), pcm_bits_per_sample(m_sample_format), m_num_channels, m_sample_rate);

    // Read all chunks before DATA.
    bool found_data = false;
    while (!found_data) {
        auto chunk_header = TRY(m_stream->read_value<RIFF::ChunkID>());
        if (chunk_header == Wav::data_chunk_id) {
            found_data = true;
        } else {
            TRY(m_stream->seek(-RIFF::chunk_id_size, SeekMode::FromCurrentPosition));
            auto chunk = TRY(m_stream->read_value<RIFF::OwnedChunk>());
            if (chunk.id() == RIFF::list_chunk_id) {
                auto maybe_list = chunk.data_stream().read_value<RIFF::OwnedList>();
                if (maybe_list.is_error()) {
                    dbgln("WAV Warning: LIST chunk invalid, error: {}", maybe_list.release_error());
                    continue;
                }

                auto list = maybe_list.release_value();
                if (list.type == Wav::info_chunk_id) {
                    auto maybe_error = load_wav_info_block(move(list.chunks));
                    if (maybe_error.is_error())
                        dbgln("WAV Warning: INFO chunk invalid, error: {}", maybe_error.release_error());

                } else {
                    dbgln("Unhandled WAV list of type {} with {} subchunks", list.type.as_ascii_string(), list.chunks.size());
                }
            } else {
                dbgln_if(AWAVLOADER_DEBUG, "Unhandled WAV chunk of type {}, size {} bytes", chunk.id().as_ascii_string(), chunk.size());
            }
        }
    }

    u32 data_size = TRY(m_stream->read_value<LittleEndian<u32>>());
    CHECK(found_data, LoaderError::Category::Format, "Found no data chunk");

    m_total_samples = data_size / block_size_bytes;

    dbgln_if(AWAVLOADER_DEBUG, "WAV data size {}, bytes per sample {}, total samples {}",
        data_size,
        block_size_bytes,
        m_total_samples);

    m_byte_offset_of_data_samples = TRY(m_stream->tell());
    return {};
}

// http://www-mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/Docs/riffmci.pdf page 23 (LIST type)
// We only recognize the relevant official metadata types; types added in later errata of RIFF are not relevant for audio.
MaybeLoaderError WavLoaderPlugin::load_wav_info_block(Vector<RIFF::OwnedChunk> info_chunks)
{
    for (auto const& chunk : info_chunks) {
        auto chunk_id = chunk.id();
        auto metadata_name = chunk_id.as_ascii_string();
        // Chunk contents are zero-terminated strings "ZSTR", so we just drop the null terminator.
        StringView metadata_text { chunk.data().trim(chunk.size() - 1) };
        // Note that we assume chunks to be unique, since that seems to almost always be the case.
        // Worst case we just drop some metadata.
        if (metadata_name == "IART"sv) {
            // Artists are combined together with semicolons, at least when you edit them in Windows File Explorer.
            auto artists = metadata_text.split_view(";"sv);
            for (auto artist : artists)
                TRY(m_metadata.add_person(Person::Role::Artist, TRY(String::from_utf8(artist))));
        } else if (metadata_name == "ICMT"sv) {
            m_metadata.comment = TRY(String::from_utf8(metadata_text));
        } else if (metadata_name == "ICOP"sv) {
            m_metadata.copyright = TRY(String::from_utf8(metadata_text));
        } else if (metadata_name == "ICRD"sv) {
            m_metadata.unparsed_time = TRY(String::from_utf8(metadata_text));
        } else if (metadata_name == "IENG"sv) {
            TRY(m_metadata.add_person(Person::Role::Engineer, TRY(String::from_utf8(metadata_text))));
        } else if (metadata_name == "IGNR"sv) {
            m_metadata.genre = TRY(String::from_utf8(metadata_text));
        } else if (metadata_name == "INAM"sv) {
            m_metadata.title = TRY(String::from_utf8(metadata_text));
        } else if (metadata_name == "IPRD"sv) {
            m_metadata.album = TRY(String::from_utf8(metadata_text));
        } else if (metadata_name == "ISFT"sv) {
            m_metadata.encoder = TRY(String::from_utf8(metadata_text));
        } else if (metadata_name == "ISRC"sv) {
            TRY(m_metadata.add_person(Person::Role::Publisher, TRY(String::from_utf8(metadata_text))));
        } else {
            TRY(m_metadata.add_miscellaneous(TRY(String::from_utf8(metadata_name)), TRY(String::from_utf8(metadata_text))));
        }
    }
    return {};
}

}
