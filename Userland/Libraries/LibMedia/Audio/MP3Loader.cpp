/*
 * Copyright (c) 2021, Arne Elster <arne@elster.li>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "MP3Loader.h"
#include "MP3HuffmanTables.h"
#include "MP3Tables.h"
#include "MP3Types.h"
#include <AK/Endian.h>
#include <AK/FixedArray.h>
#include <LibCore/File.h>

namespace Audio {

DSP::MDCT<12> MP3LoaderPlugin::s_mdct_12;
DSP::MDCT<36> MP3LoaderPlugin::s_mdct_36;

MP3LoaderPlugin::MP3LoaderPlugin(NonnullOwnPtr<SeekableStream> stream)
    : LoaderPlugin(move(stream))
{
}

MaybeLoaderError MP3LoaderPlugin::skip_id3(SeekableStream& stream)
{
    // FIXME: This is a bit of a hack until we have a proper ID3 reader and MP3 demuxer.
    // Based on https://mutagen-specs.readthedocs.io/en/latest/id3/id3v2.2.html
    char identifier_buffer[3] = { 0, 0, 0 };
    auto read_identifier = StringView(TRY(stream.read_some({ &identifier_buffer[0], sizeof(identifier_buffer) })));
    if (read_identifier == "ID3"sv) {
        [[maybe_unused]] auto version = TRY(stream.read_value<u8>());
        [[maybe_unused]] auto revision = TRY(stream.read_value<u8>());
        [[maybe_unused]] auto flags = TRY(stream.read_value<u8>());
        auto size = 0;
        for (auto i = 0; i < 4; i++) {
            // Each byte has a zeroed most significant bit to prevent it from looking like a sync code.
            auto byte = TRY(stream.read_value<u8>());
            size <<= 7;
            size |= byte & 0x7F;
        }
        TRY(stream.seek(size, SeekMode::FromCurrentPosition));
    } else if (read_identifier != "TAG"sv) {
        MUST(stream.seek(-static_cast<int>(read_identifier.length()), SeekMode::FromCurrentPosition));
    }
    return {};
}

bool MP3LoaderPlugin::sniff(SeekableStream& stream)
{
    auto skip_id3_result = skip_id3(stream);
    if (skip_id3_result.is_error())
        return false;
    return !synchronize_and_read_header(stream, 0).is_error();
}

ErrorOr<NonnullOwnPtr<LoaderPlugin>, LoaderError> MP3LoaderPlugin::create(NonnullOwnPtr<SeekableStream> stream)
{
    auto loader = make<MP3LoaderPlugin>(move(stream));
    TRY(loader->initialize());
    return loader;
}

MaybeLoaderError MP3LoaderPlugin::initialize()
{
    TRY(build_seek_table());

    TRY(seek(0));
    auto header = TRY(synchronize_and_read_header());

    m_sample_rate = header.samplerate;
    m_num_channels = header.channel_count();
    m_loaded_samples = 0;

    TRY(seek(0));

    return {};
}

MaybeLoaderError MP3LoaderPlugin::reset()
{
    TRY(seek(0));
    m_synthesis_buffer = {};
    m_loaded_samples = 0;
    TRY(m_bit_reservoir.discard(m_bit_reservoir.used_buffer_size()));
    return {};
}

MaybeLoaderError MP3LoaderPlugin::seek(int const position)
{
    auto seek_entry = m_seek_table.seek_point_before(position);
    if (seek_entry.has_value()) {
        TRY(m_stream->seek(seek_entry->byte_offset, SeekMode::SetPosition));
        m_loaded_samples = seek_entry->sample_index;
    }
    m_synthesis_buffer = {};
    TRY(m_bit_reservoir.discard(m_bit_reservoir.used_buffer_size()));
    return {};
}

ErrorOr<Vector<FixedArray<Sample>>, LoaderError> MP3LoaderPlugin::load_chunks(size_t samples_to_read_from_input)
{
    int samples_to_read = samples_to_read_from_input;
    Vector<FixedArray<Sample>> frames;
    while (samples_to_read > 0) {
        FixedArray<Sample> samples = TRY(FixedArray<Sample>::create(MP3::frame_size));

        auto maybe_frame = read_next_frame();
        if (maybe_frame.is_error()) {
            if (m_stream->is_eof())
                return Vector<FixedArray<Sample>> {};
            return maybe_frame.release_error();
        }
        auto frame = maybe_frame.release_value();

        bool const is_stereo = frame.header.channel_count() == 2;
        size_t current_frame_read = 0;
        for (; current_frame_read < MP3::granule_size; current_frame_read++) {
            auto const left_sample = frame.channels[0].granules[0].pcm[current_frame_read / 32][current_frame_read % 32];
            auto const right_sample = is_stereo ? frame.channels[1].granules[0].pcm[current_frame_read / 32][current_frame_read % 32] : left_sample;
            samples[current_frame_read] = Sample { left_sample, right_sample };
            samples_to_read--;
        }
        for (; current_frame_read < MP3::frame_size; current_frame_read++) {
            auto const left_sample = frame.channels[0].granules[1].pcm[(current_frame_read - MP3::granule_size) / 32][(current_frame_read - MP3::granule_size) % 32];
            auto const right_sample = is_stereo ? frame.channels[1].granules[1].pcm[(current_frame_read - MP3::granule_size) / 32][(current_frame_read - MP3::granule_size) % 32] : left_sample;
            samples[current_frame_read] = Sample { left_sample, right_sample };
            samples_to_read--;
        }
        m_loaded_samples += samples.size();
        TRY(frames.try_append(move(samples)));
    }

    return frames;
}

MaybeLoaderError MP3LoaderPlugin::build_seek_table()
{
    VERIFY(MUST(m_stream->tell()) == 0);
    TRY(skip_id3(*m_stream));

    int sample_count = 0;
    size_t frame_count = 0;
    m_seek_table = {};

    while (true) {
        auto error_or_header = synchronize_and_read_header();
        if (error_or_header.is_error())
            break;

        if (frame_count % 10 == 0) {
            auto frame_pos = TRY(m_stream->tell()) - error_or_header.value().header_size;
            TRY(m_seek_table.insert_seek_point({ static_cast<u64>(sample_count), frame_pos }));
        }

        frame_count++;
        sample_count += MP3::frame_size;

        TRY(m_stream->seek(error_or_header.value().frame_size - error_or_header.value().header_size, SeekMode::FromCurrentPosition));
    }
    m_total_samples = sample_count;
    return {};
}

ErrorOr<MP3::Header, LoaderError> MP3LoaderPlugin::read_header(SeekableStream& stream, size_t sample_index)
{
    auto bitstream = BigEndianInputBitStream(MaybeOwned<Stream>(stream));
    if (TRY(bitstream.read_bits(4)) != 0xF)
        return LoaderError { LoaderError::Category::Format, sample_index, "Frame header did not start with sync code." };
    MP3::Header header;
    header.id = TRY(bitstream.read_bit());
    header.layer = MP3::Tables::LayerNumberLookup[TRY(bitstream.read_bits(2))];
    if (header.layer <= 0)
        return LoaderError { LoaderError::Category::Format, sample_index, "Frame header contains invalid layer number." };
    header.protection_bit = TRY(bitstream.read_bit());
    header.bitrate = MP3::Tables::BitratesPerLayerLookup[header.layer - 1][TRY(bitstream.read_bits(4))];
    if (header.bitrate <= 0)
        return LoaderError { LoaderError::Category::Format, sample_index, "Frame header contains invalid bitrate." };
    header.samplerate = MP3::Tables::SampleratesLookup[TRY(bitstream.read_bits(2))];
    if (header.samplerate <= 0)
        return LoaderError { LoaderError::Category::Format, sample_index, "Frame header contains invalid samplerate." };
    header.padding_bit = TRY(bitstream.read_bit());
    header.private_bit = TRY(bitstream.read_bit());
    header.mode = static_cast<MP3::Mode>(TRY(bitstream.read_bits(2)));
    header.mode_extension = static_cast<MP3::ModeExtension>(TRY(bitstream.read_bits(2)));
    header.copyright_bit = TRY(bitstream.read_bit());
    header.original_bit = TRY(bitstream.read_bit());
    header.emphasis = static_cast<MP3::Emphasis>(TRY(bitstream.read_bits(2)));
    header.header_size = 4;
    if (!header.protection_bit) {
        header.crc16 = TRY(bitstream.read_bits<u16>(16));
        header.header_size += 2;
    }
    header.frame_size = 144 * header.bitrate * 1000 / header.samplerate + header.padding_bit;
    header.slot_count = header.frame_size - ((header.channel_count() == 2 ? 32 : 17) + header.header_size);
    return header;
}

ErrorOr<MP3::Header, LoaderError> MP3LoaderPlugin::synchronize_and_read_header(SeekableStream& stream, size_t sample_index)
{
    while (!stream.is_eof()) {
        bool last_was_all_set = false;

        while (!stream.is_eof()) {
            u8 byte = TRY(stream.read_value<u8>());
            if (last_was_all_set && (byte & 0xF0) == 0xF0) {
                // Seek back, since there is still data we have not consumed within the current byte.
                // read_header() will consume and check these 4 bits itself and then continue reading
                // the rest of the data from there.
                TRY(stream.seek(-1, SeekMode::FromCurrentPosition));
                break;
            }
            last_was_all_set = byte == 0xFF;
        }

        auto header_start = TRY(stream.tell());
        auto header_result = read_header(stream, sample_index);
        if (header_result.is_error() || header_result.value().id != 1 || header_result.value().layer != 3) {
            TRY(stream.seek(header_start, SeekMode::SetPosition));
            continue;
        }
        return header_result.value();
    }
    return LoaderError { LoaderError::Category::Format, sample_index, "Failed to synchronize." };
}

ErrorOr<MP3::Header, LoaderError> MP3LoaderPlugin::synchronize_and_read_header()
{
    return MP3LoaderPlugin::synchronize_and_read_header(*m_stream, m_loaded_samples);
}

ErrorOr<MP3::MP3Frame, LoaderError> MP3LoaderPlugin::read_next_frame()
{
    return read_frame_data(TRY(synchronize_and_read_header()));
}

ErrorOr<MP3::MP3Frame, LoaderError> MP3LoaderPlugin::read_frame_data(MP3::Header const& header)
{
    MP3::MP3Frame frame { header };

    TRY(read_side_information(frame));

    auto maybe_buffer = ByteBuffer::create_uninitialized(header.slot_count);
    if (maybe_buffer.is_error())
        return LoaderError { LoaderError::Category::IO, m_loaded_samples, "Out of memory" };
    auto& buffer = maybe_buffer.value();

    size_t old_reservoir_size = m_bit_reservoir.used_buffer_size();
    TRY(m_stream->read_until_filled(buffer));
    TRY(m_bit_reservoir.write_until_depleted(buffer));

    // If we don't have enough data in the reservoir to process this frame, skip it (but keep the data).
    if (old_reservoir_size < static_cast<size_t>(frame.main_data_begin))
        return frame;

    TRY(m_bit_reservoir.discard(old_reservoir_size - frame.main_data_begin));

    BigEndianInputBitStream reservoir_stream { MaybeOwned<Stream>(m_bit_reservoir) };

    for (size_t granule_index = 0; granule_index < 2; granule_index++) {
        for (size_t channel_index = 0; channel_index < header.channel_count(); channel_index++) {
            size_t scale_factor_size = TRY(read_scale_factors(frame, reservoir_stream, granule_index, channel_index));
            TRY(read_huffman_data(frame, reservoir_stream, granule_index, channel_index, scale_factor_size));
            if (frame.channels[channel_index].granules[granule_index].block_type == MP3::BlockType::Short) {
                reorder_samples(frame.channels[channel_index].granules[granule_index], frame.header.samplerate);

                // Only reduce alias for lowest 2 bands as they're long.
                // Afaik this is not mentioned in the ISO spec, but it is addressed in the
                // changelog for the ISO compliance tests.
                if (frame.channels[channel_index].granules[granule_index].mixed_block_flag)
                    reduce_alias(frame.channels[channel_index].granules[granule_index], 36);
            } else {
                reduce_alias(frame.channels[channel_index].granules[granule_index]);
            }
        }

        if (header.mode == MP3::Mode::JointStereo) {
            process_stereo(frame, granule_index);
        }
    }

    for (size_t granule_index = 0; granule_index < 2; granule_index++) {
        for (size_t channel_index = 0; channel_index < header.channel_count(); channel_index++) {
            auto& granule = frame.channels[channel_index].granules[granule_index];

            for (size_t i = 0; i < MP3::granule_size; i += 18) {
                MP3::BlockType block_type = granule.block_type;
                if (i < 36 && granule.mixed_block_flag) {
                    // ISO/IEC 11172-3: if mixed_block_flag is set, the lowest two subbands are transformed with normal window.
                    block_type = MP3::BlockType::Normal;
                }

                Array<float, 36> output;
                transform_samples_to_time(granule.samples, i, output, block_type);

                int const subband_index = i / 18;
                for (size_t sample_index = 0; sample_index < 18; sample_index++) {
                    // overlap add
                    granule.filter_bank_input[subband_index][sample_index] = output[sample_index] + m_last_values[channel_index][subband_index][sample_index];
                    m_last_values[channel_index][subband_index][sample_index] = output[sample_index + 18];

                    // frequency inversion
                    if (subband_index % 2 == 1 && sample_index % 2 == 1)
                        granule.filter_bank_input[subband_index][sample_index] *= -1;
                }
            }
        }
    }

    Array<float, 32> in_samples;
    for (size_t channel_index = 0; channel_index < frame.header.channel_count(); channel_index++) {
        for (size_t granule_index = 0; granule_index < 2; granule_index++) {
            auto& granule = frame.channels[channel_index].granules[granule_index];
            for (size_t sample_index = 0; sample_index < 18; sample_index++) {
                for (size_t band_index = 0; band_index < 32; band_index++) {
                    in_samples[band_index] = granule.filter_bank_input[band_index][sample_index];
                }
                synthesis(m_synthesis_buffer[channel_index], in_samples, granule.pcm[sample_index]);
            }
        }
    }

    return frame;
}

MaybeLoaderError MP3LoaderPlugin::read_side_information(MP3::MP3Frame& frame)
{
    auto bitstream = BigEndianInputBitStream(MaybeOwned<Stream>(*m_stream));

    frame.main_data_begin = TRY(bitstream.read_bits(9));

    if (frame.header.channel_count() == 1) {
        frame.private_bits = TRY(bitstream.read_bits(5));
    } else {
        frame.private_bits = TRY(bitstream.read_bits(3));
    }

    for (size_t channel_index = 0; channel_index < frame.header.channel_count(); channel_index++) {
        for (size_t scale_factor_selection_info_band = 0; scale_factor_selection_info_band < 4; scale_factor_selection_info_band++) {
            frame.channels[channel_index].scale_factor_selection_info[scale_factor_selection_info_band] = TRY(bitstream.read_bit());
        }
    }

    for (size_t granule_index = 0; granule_index < 2; granule_index++) {
        for (size_t channel_index = 0; channel_index < frame.header.channel_count(); channel_index++) {
            auto& granule = frame.channels[channel_index].granules[granule_index];
            granule.part_2_3_length = TRY(bitstream.read_bits(12));
            granule.big_values = TRY(bitstream.read_bits(9));
            granule.global_gain = TRY(bitstream.read_bits(8));
            granule.scalefac_compress = TRY(bitstream.read_bits(4));
            granule.window_switching_flag = TRY(bitstream.read_bit());
            if (granule.window_switching_flag) {
                granule.block_type = static_cast<MP3::BlockType>(TRY(bitstream.read_bits(2)));
                granule.mixed_block_flag = TRY(bitstream.read_bit());
                for (size_t region = 0; region < 2; region++)
                    granule.table_select[region] = TRY(bitstream.read_bits(5));
                for (size_t window = 0; window < 3; window++)
                    granule.sub_block_gain[window] = TRY(bitstream.read_bits(3));
                granule.region0_count = (granule.block_type == MP3::BlockType::Short && !granule.mixed_block_flag) ? 8 : 7;
                granule.region1_count = 36;
            } else {
                for (size_t region = 0; region < 3; region++)
                    granule.table_select[region] = TRY(bitstream.read_bits(5));
                granule.region0_count = TRY(bitstream.read_bits(4));
                granule.region1_count = TRY(bitstream.read_bits(3));
            }
            granule.preflag = TRY(bitstream.read_bit());
            granule.scalefac_scale = TRY(bitstream.read_bit());
            granule.count1table_select = TRY(bitstream.read_bit());
        }
    }
    return {};
}

// From ISO/IEC 11172-3 (2.4.3.4.7.1)
Array<float, MP3::granule_size> MP3LoaderPlugin::calculate_frame_exponents(MP3::MP3Frame const& frame, size_t granule_index, size_t channel_index)
{
    Array<float, MP3::granule_size> exponents;

    auto fill_band = [&exponents](float exponent, size_t start, size_t end) {
        for (size_t j = start; j <= end; j++) {
            exponents[j] = exponent;
        }
    };

    auto const& channel = frame.channels[channel_index];
    auto const& granule = frame.channels[channel_index].granules[granule_index];

    auto const scale_factor_bands = get_scalefactor_bands(granule, frame.header.samplerate);
    float const scale_factor_multiplier = granule.scalefac_scale ? 1 : 0.5;
    int const gain = granule.global_gain - 210;

    if (granule.block_type != MP3::BlockType::Short) {
        for (size_t band_index = 0; band_index < 22; band_index++) {
            float const exponent = gain / 4.0f - (scale_factor_multiplier * (channel.scale_factors[band_index] + granule.preflag * MP3::Tables::Pretab[band_index]));
            fill_band(AK::pow<float>(2.0, exponent), scale_factor_bands[band_index].start, scale_factor_bands[band_index].end);
        }
    } else {
        size_t band_index = 0;
        size_t sample_count = 0;

        if (granule.mixed_block_flag) {
            while (sample_count < 36) {
                float const exponent = gain / 4.0f - (scale_factor_multiplier * (channel.scale_factors[band_index] + granule.preflag * MP3::Tables::Pretab[band_index]));
                fill_band(AK::pow<float>(2.0, exponent), scale_factor_bands[band_index].start, scale_factor_bands[band_index].end);
                sample_count += scale_factor_bands[band_index].width;
                band_index++;
            }
        }

        float const gain0 = (gain - 8 * granule.sub_block_gain[0]) / 4.0;
        float const gain1 = (gain - 8 * granule.sub_block_gain[1]) / 4.0;
        float const gain2 = (gain - 8 * granule.sub_block_gain[2]) / 4.0;

        while (sample_count < MP3::granule_size && band_index < scale_factor_bands.size()) {
            float const exponent0 = gain0 - (scale_factor_multiplier * channel.scale_factors[band_index + 0]);
            float const exponent1 = gain1 - (scale_factor_multiplier * channel.scale_factors[band_index + 1]);
            float const exponent2 = gain2 - (scale_factor_multiplier * channel.scale_factors[band_index + 2]);

            fill_band(AK::pow<float>(2.0, exponent0), scale_factor_bands[band_index + 0].start, scale_factor_bands[band_index + 0].end);
            sample_count += scale_factor_bands[band_index + 0].width;
            fill_band(AK::pow<float>(2.0, exponent1), scale_factor_bands[band_index + 1].start, scale_factor_bands[band_index + 1].end);
            sample_count += scale_factor_bands[band_index + 1].width;
            fill_band(AK::pow<float>(2.0, exponent2), scale_factor_bands[band_index + 2].start, scale_factor_bands[band_index + 2].end);
            sample_count += scale_factor_bands[band_index + 2].width;

            band_index += 3;
        }

        while (sample_count < MP3::granule_size)
            exponents[sample_count++] = 0;
    }
    return exponents;
}

ErrorOr<size_t, LoaderError> MP3LoaderPlugin::read_scale_factors(MP3::MP3Frame& frame, BigEndianInputBitStream& reservoir, size_t granule_index, size_t channel_index)
{
    auto& channel = frame.channels[channel_index];
    auto const& granule = channel.granules[granule_index];
    size_t band_index = 0;
    size_t bits_read = 0;

    if (granule.window_switching_flag && granule.block_type == MP3::BlockType::Short) {
        if (granule.mixed_block_flag) {
            for (size_t i = 0; i < 8; i++) {
                auto const bits = MP3::Tables::ScalefacCompressSlen1[granule.scalefac_compress];
                channel.scale_factors[band_index++] = TRY(reservoir.read_bits(bits));
                bits_read += bits;
            }
            for (size_t i = 3; i < 12; i++) {
                auto const bits = i <= 5 ? MP3::Tables::ScalefacCompressSlen1[granule.scalefac_compress] : MP3::Tables::ScalefacCompressSlen2[granule.scalefac_compress];
                channel.scale_factors[band_index++] = TRY(reservoir.read_bits(bits));
                channel.scale_factors[band_index++] = TRY(reservoir.read_bits(bits));
                channel.scale_factors[band_index++] = TRY(reservoir.read_bits(bits));
                bits_read += 3 * bits;
            }
        } else {
            for (size_t i = 0; i < 12; i++) {
                auto const bits = i <= 5 ? MP3::Tables::ScalefacCompressSlen1[granule.scalefac_compress] : MP3::Tables::ScalefacCompressSlen2[granule.scalefac_compress];
                channel.scale_factors[band_index++] = TRY(reservoir.read_bits(bits));
                channel.scale_factors[band_index++] = TRY(reservoir.read_bits(bits));
                channel.scale_factors[band_index++] = TRY(reservoir.read_bits(bits));
                bits_read += 3 * bits;
            }
        }
        channel.scale_factors[band_index++] = 0;
        channel.scale_factors[band_index++] = 0;
        channel.scale_factors[band_index++] = 0;
    } else {
        if ((channel.scale_factor_selection_info[0] == 0) || (granule_index == 0)) {
            for (band_index = 0; band_index < 6; band_index++) {
                auto const bits = MP3::Tables::ScalefacCompressSlen1[granule.scalefac_compress];
                channel.scale_factors[band_index] = TRY(reservoir.read_bits(bits));
                bits_read += bits;
            }
        }
        if ((channel.scale_factor_selection_info[1] == 0) || (granule_index == 0)) {
            for (band_index = 6; band_index < 11; band_index++) {
                auto const bits = MP3::Tables::ScalefacCompressSlen1[granule.scalefac_compress];
                channel.scale_factors[band_index] = TRY(reservoir.read_bits(bits));
                bits_read += bits;
            }
        }
        if ((channel.scale_factor_selection_info[2] == 0) || (granule_index == 0)) {
            for (band_index = 11; band_index < 16; band_index++) {
                auto const bits = MP3::Tables::ScalefacCompressSlen2[granule.scalefac_compress];
                channel.scale_factors[band_index] = TRY(reservoir.read_bits(bits));
                bits_read += bits;
            }
        }
        if ((channel.scale_factor_selection_info[3] == 0) || (granule_index == 0)) {
            for (band_index = 16; band_index < 21; band_index++) {
                auto const bits = MP3::Tables::ScalefacCompressSlen2[granule.scalefac_compress];
                channel.scale_factors[band_index] = TRY(reservoir.read_bits(bits));
                bits_read += bits;
            }
        }
        channel.scale_factors[21] = 0;
    }

    return bits_read;
}

MaybeLoaderError MP3LoaderPlugin::read_huffman_data(MP3::MP3Frame& frame, BigEndianInputBitStream& reservoir, size_t granule_index, size_t channel_index, size_t granule_bits_read)
{
    auto const exponents = calculate_frame_exponents(frame, granule_index, channel_index);
    auto& granule = frame.channels[channel_index].granules[granule_index];

    auto const scale_factor_bands = get_scalefactor_bands(granule, frame.header.samplerate);
    size_t const scale_factor_band_index1 = granule.region0_count + 1;
    size_t const scale_factor_band_index2 = min(scale_factor_bands.size() - 1, scale_factor_band_index1 + granule.region1_count + 1);

    bool const is_short_granule = granule.window_switching_flag && granule.block_type == MP3::BlockType::Short;
    size_t const region1_start = is_short_granule ? 36 : scale_factor_bands[scale_factor_band_index1].start;
    size_t const region2_start = is_short_granule ? MP3::granule_size : scale_factor_bands[scale_factor_band_index2].start;

    auto requantize = [](int const sample, float const exponent) -> float {
        int const sign = sample < 0 ? -1 : 1;
        int const magnitude = AK::abs(sample);
        return sign * AK::pow<float>(static_cast<float>(magnitude), 4 / 3.0) * exponent;
    };

    size_t count = 0;

    // 2.4.3.4.6: "Decoding is done until all Huffman code bits have been decoded
    //             or until quantized values representing 576 frequency lines have been decoded,
    //             whichever comes first."
    auto max_count = min(granule.big_values * 2, MP3::granule_size);

    for (; count < max_count; count += 2) {
        MP3::Tables::Huffman::HuffmanTreeXY const* tree = nullptr;

        if (count < region1_start) {
            tree = &MP3::Tables::Huffman::HuffmanTreesXY[granule.table_select[0]];
        } else if (count < region2_start) {
            tree = &MP3::Tables::Huffman::HuffmanTreesXY[granule.table_select[1]];
        } else {
            tree = &MP3::Tables::Huffman::HuffmanTreesXY[granule.table_select[2]];
        }

        if (!tree || tree->nodes.is_empty()) {
            return LoaderError { LoaderError::Category::Format, m_loaded_samples, "Frame references invalid huffman table." };
        }

        // Assumption: There's enough bits to read. 32 is just a placeholder for "unlimited".
        // There are no 32 bit long huffman codes in the tables.
        auto const entry = MP3::Tables::Huffman::huffman_decode(reservoir, tree->nodes, 32);
        granule_bits_read += entry.bits_read;
        if (!entry.code.has_value())
            return LoaderError { LoaderError::Category::Format, m_loaded_samples, "Frame contains invalid huffman data." };
        int x = entry.code->symbol.x;
        int y = entry.code->symbol.y;

        if (x == 15 && tree->linbits > 0) {
            x += TRY(reservoir.read_bits(tree->linbits));
            granule_bits_read += tree->linbits;
        }
        if (x != 0) {
            if (TRY(reservoir.read_bit()))
                x = -x;
            granule_bits_read++;
        }

        if (y == 15 && tree->linbits > 0) {
            y += TRY(reservoir.read_bits(tree->linbits));
            granule_bits_read += tree->linbits;
        }
        if (y != 0) {
            if (TRY(reservoir.read_bit()))
                y = -y;
            granule_bits_read++;
        }

        granule.samples[count + 0] = requantize(x, exponents[count + 0]);
        granule.samples[count + 1] = requantize(y, exponents[count + 1]);
    }

    ReadonlySpan<MP3::Tables::Huffman::HuffmanNode<MP3::Tables::Huffman::HuffmanVWXY>> count1table = granule.count1table_select ? MP3::Tables::Huffman::TreeB : MP3::Tables::Huffman::TreeA;

    // count1 is not known. We have to read huffman encoded values
    // until we've exhausted the granule's bits. We know the size of
    // the granule from part2_3_length, which is the number of bits
    // used for scalefactors and huffman data (in the granule).
    while (granule_bits_read < granule.part_2_3_length && count <= MP3::granule_size - 4) {
        auto const entry = MP3::Tables::Huffman::huffman_decode(reservoir, count1table, granule.part_2_3_length - granule_bits_read);
        granule_bits_read += entry.bits_read;
        if (!entry.code.has_value())
            return LoaderError { LoaderError::Category::Format, m_loaded_samples, "Frame contains invalid huffman data." };
        int v = entry.code->symbol.v;
        if (v != 0) {
            if (granule_bits_read >= granule.part_2_3_length)
                break;
            if (TRY(reservoir.read_bit()))
                v = -v;
            granule_bits_read++;
        }
        int w = entry.code->symbol.w;
        if (w != 0) {
            if (granule_bits_read >= granule.part_2_3_length)
                break;
            if (TRY(reservoir.read_bit()))
                w = -w;
            granule_bits_read++;
        }
        int x = entry.code->symbol.x;
        if (x != 0) {
            if (granule_bits_read >= granule.part_2_3_length)
                break;
            if (TRY(reservoir.read_bit()))
                x = -x;
            granule_bits_read++;
        }
        int y = entry.code->symbol.y;
        if (y != 0) {
            if (granule_bits_read >= granule.part_2_3_length)
                break;
            if (TRY(reservoir.read_bit()))
                y = -y;
            granule_bits_read++;
        }

        granule.samples[count + 0] = requantize(v, exponents[count + 0]);
        granule.samples[count + 1] = requantize(w, exponents[count + 1]);
        granule.samples[count + 2] = requantize(x, exponents[count + 2]);
        granule.samples[count + 3] = requantize(y, exponents[count + 3]);

        count += 4;
    }

    if (granule_bits_read > granule.part_2_3_length) {
        return LoaderError { LoaderError::Category::Format, m_loaded_samples, "Read too many bits from bit reservoir." };
    }

    // 2.4.3.4.6: "If there are more Huffman code bits than necessary to decode 576 values
    //             they are regarded as stuffing bits and discarded."
    for (size_t i = granule_bits_read; i < granule.part_2_3_length; i++) {
        TRY(reservoir.read_bit());
    }

    return {};
}

void MP3LoaderPlugin::reorder_samples(MP3::Granule& granule, u32 sample_rate)
{
    float tmp[MP3::granule_size] = {};
    size_t band_index = 0;
    size_t subband_index = 0;

    auto scale_factor_bands = get_scalefactor_bands(granule, sample_rate);

    if (granule.mixed_block_flag) {
        while (subband_index < 36) {
            for (size_t frequency_line_index = 0; frequency_line_index < scale_factor_bands[band_index].width; frequency_line_index++) {
                tmp[subband_index] = granule.samples[subband_index];
                subband_index++;
            }
            band_index++;
        }
    }

    while (subband_index < MP3::granule_size && band_index <= 36) {
        for (size_t frequency_line_index = 0; frequency_line_index < scale_factor_bands[band_index].width; frequency_line_index++) {
            tmp[subband_index++] = granule.samples[scale_factor_bands[band_index + 0].start + frequency_line_index];
            tmp[subband_index++] = granule.samples[scale_factor_bands[band_index + 1].start + frequency_line_index];
            tmp[subband_index++] = granule.samples[scale_factor_bands[band_index + 2].start + frequency_line_index];
        }
        band_index += 3;
    }

    for (size_t i = 0; i < MP3::granule_size; i++)
        granule.samples[i] = tmp[i];
}

void MP3LoaderPlugin::reduce_alias(MP3::Granule& granule, size_t max_subband_index)
{
    for (size_t subband = 0; subband < max_subband_index - 18; subband += 18) {
        for (size_t i = 0; i < 8; i++) {
            size_t const idx1 = subband + 17 - i;
            size_t const idx2 = subband + 18 + i;
            auto const d1 = granule.samples[idx1];
            auto const d2 = granule.samples[idx2];
            granule.samples[idx1] = d1 * MP3::Tables::AliasReductionCs[i] - d2 * MP3::Tables::AliasReductionCa[i];
            granule.samples[idx2] = d2 * MP3::Tables::AliasReductionCs[i] + d1 * MP3::Tables::AliasReductionCa[i];
        }
    }
}

void MP3LoaderPlugin::process_stereo(MP3::MP3Frame& frame, size_t granule_index)
{
    size_t band_index_ms_start = 0;
    size_t band_index_ms_end = 0;
    size_t band_index_intensity_start = 0;
    size_t band_index_intensity_end = 0;
    auto& granule_left = frame.channels[0].granules[granule_index];
    auto& granule_right = frame.channels[1].granules[granule_index];

    auto get_last_nonempty_band = [](Span<float> samples, ReadonlySpan<MP3::Tables::ScaleFactorBand> bands) -> size_t {
        size_t last_nonempty_band = 0;

        for (size_t i = 0; i < bands.size(); i++) {
            bool is_empty = true;
            for (size_t l = bands[i].start; l < bands[i].end; l++) {
                if (samples[l] != 0) {
                    is_empty = false;
                    break;
                }
            }
            if (!is_empty)
                last_nonempty_band = i;
        }

        return last_nonempty_band;
    };

    auto process_ms_stereo = [&](MP3::Tables::ScaleFactorBand const& band) {
        float const SQRT_2 = AK::sqrt(2.0);
        for (size_t i = band.start; i <= band.end; i++) {
            float const m = granule_left.samples[i];
            float const s = granule_right.samples[i];
            granule_left.samples[i] = (m + s) / SQRT_2;
            granule_right.samples[i] = (m - s) / SQRT_2;
        }
    };

    auto process_intensity_stereo = [&](MP3::Tables::ScaleFactorBand const& band, float intensity_stereo_ratio) {
        for (size_t i = band.start; i <= band.end; i++) {
            // Superflous empty scale factor band.
            if (i >= MP3::granule_size)
                continue;
            float const sample_left = granule_left.samples[i];
            float const coeff_l = intensity_stereo_ratio / (1 + intensity_stereo_ratio);
            float const coeff_r = 1 / (1 + intensity_stereo_ratio);
            granule_left.samples[i] = sample_left * coeff_l;
            granule_right.samples[i] = sample_left * coeff_r;
        }
    };

    auto scale_factor_bands = get_scalefactor_bands(granule_right, frame.header.samplerate);

    if (has_flag(frame.header.mode_extension, MP3::ModeExtension::MsStereo)) {
        band_index_ms_start = 0;
        band_index_ms_end = scale_factor_bands.size();
    }

    if (has_flag(frame.header.mode_extension, MP3::ModeExtension::IntensityStereo)) {
        band_index_intensity_start = get_last_nonempty_band(granule_right.samples, scale_factor_bands);
        band_index_intensity_end = scale_factor_bands.size();
        band_index_ms_end = band_index_intensity_start;
    }

    for (size_t band_index = band_index_ms_start; band_index < band_index_ms_end; band_index++) {
        process_ms_stereo(scale_factor_bands[band_index]);
    }

    for (size_t band_index = band_index_intensity_start; band_index < band_index_intensity_end; band_index++) {
        auto const intensity_stereo_position = frame.channels[1].scale_factors[band_index];
        if (intensity_stereo_position == 7) {
            if (has_flag(frame.header.mode_extension, MP3::ModeExtension::MsStereo))
                process_ms_stereo(scale_factor_bands[band_index]);
            continue;
        }
        float const intensity_stereo_ratio = AK::tan(intensity_stereo_position * AK::Pi<float> / 12);
        process_intensity_stereo(scale_factor_bands[band_index], intensity_stereo_ratio);
    }
}

void MP3LoaderPlugin::transform_samples_to_time(Array<float, MP3::granule_size> const& input, size_t input_offset, Array<float, 36>& output, MP3::BlockType block_type)
{
    if (block_type == MP3::BlockType::Short) {
        size_t const N = 12;
        Array<float, N * 3> temp_out;
        Array<float, N / 2> temp_in;

        for (size_t k = 0; k < N / 2; k++)
            temp_in[k] = input[input_offset + 3 * k + 0];
        s_mdct_12.transform(temp_in, Span<float>(temp_out).slice(0, N));
        for (size_t i = 0; i < N; i++)
            temp_out[i + 0] *= MP3::Tables::WindowBlockTypeShort[i];

        for (size_t k = 0; k < N / 2; k++)
            temp_in[k] = input[input_offset + 3 * k + 1];
        s_mdct_12.transform(temp_in, Span<float>(temp_out).slice(12, N));
        for (size_t i = 0; i < N; i++)
            temp_out[i + 12] *= MP3::Tables::WindowBlockTypeShort[i];

        for (size_t k = 0; k < N / 2; k++)
            temp_in[k] = input[input_offset + 3 * k + 2];
        s_mdct_12.transform(temp_in, Span<float>(temp_out).slice(24, N));
        for (size_t i = 0; i < N; i++)
            temp_out[i + 24] *= MP3::Tables::WindowBlockTypeShort[i];

        Span<float> idmct1 = Span<float>(temp_out).slice(0, 12);
        Span<float> idmct2 = Span<float>(temp_out).slice(12, 12);
        Span<float> idmct3 = Span<float>(temp_out).slice(24, 12);
        for (size_t i = 0; i < 6; i++)
            output[i] = 0;
        for (size_t i = 6; i < 12; i++)
            output[i] = idmct1[i - 6];
        for (size_t i = 12; i < 18; i++)
            output[i] = idmct1[i - 6] + idmct2[i - 12];
        for (size_t i = 18; i < 24; i++)
            output[i] = idmct2[i - 12] + idmct3[i - 18];
        for (size_t i = 24; i < 30; i++)
            output[i] = idmct3[i - 18];
        for (size_t i = 30; i < 36; i++)
            output[i] = 0;

    } else {
        s_mdct_36.transform(ReadonlySpan<float>(input).slice(input_offset, 18), output);
        for (size_t i = 0; i < 36; i++) {
            switch (block_type) {
            case MP3::BlockType::Normal:
                output[i] *= MP3::Tables::WindowBlockTypeNormal[i];
                break;
            case MP3::BlockType::Start:
                output[i] *= MP3::Tables::WindowBlockTypeStart[i];
                break;
            case MP3::BlockType::End:
                output[i] *= MP3::Tables::WindowBlockTypeEnd[i];
                break;
            case MP3::BlockType::Short:
                VERIFY_NOT_REACHED();
                break;
            }
        }
    }
}

// ISO/IEC 11172-3 (Figure A.2)
void MP3LoaderPlugin::synthesis(Array<float, 1024>& V, Array<float, 32>& samples, Array<float, 32>& result)
{
    for (size_t i = 1023; i >= 64; i--) {
        V[i] = V[i - 64];
    }

    for (size_t i = 0; i < 64; i++) {
        V[i] = 0;
        for (size_t k = 0; k < 32; k++) {
            float const N = MP3::Tables::SynthesisSubbandFilterCoefficients[i][k];
            V[i] += N * samples[k];
        }
    }

    Array<float, 512> U;
    for (size_t i = 0; i < 8; i++) {
        for (size_t j = 0; j < 32; j++) {
            U[i * 64 + j] = V[i * 128 + j];
            U[i * 64 + 32 + j] = V[i * 128 + 96 + j];
        }
    }

    Array<float, 512> W;
    for (size_t i = 0; i < 512; i++) {
        W[i] = U[i] * MP3::Tables::WindowSynthesis[i];
    }

    for (size_t j = 0; j < 32; j++) {
        result[j] = 0;
        for (size_t k = 0; k < 16; k++) {
            result[j] += W[j + 32 * k];
        }
    }
}

ReadonlySpan<MP3::Tables::ScaleFactorBand> MP3LoaderPlugin::get_scalefactor_bands(MP3::Granule const& granule, int samplerate)
{
    switch (granule.block_type) {
    case MP3::BlockType::Short:
        switch (samplerate) {
        case 32000:
            return granule.mixed_block_flag ? MP3::Tables::ScaleFactorBandMixed32000 : MP3::Tables::ScaleFactorBandShort32000;
        case 44100:
            return granule.mixed_block_flag ? MP3::Tables::ScaleFactorBandMixed44100 : MP3::Tables::ScaleFactorBandShort44100;
        case 48000:
            return granule.mixed_block_flag ? MP3::Tables::ScaleFactorBandMixed48000 : MP3::Tables::ScaleFactorBandShort48000;
        }
        break;
    case MP3::BlockType::Normal:
        [[fallthrough]];
    case MP3::BlockType::Start:
        [[fallthrough]];
    case MP3::BlockType::End:
        switch (samplerate) {
        case 32000:
            return MP3::Tables::ScaleFactorBandLong32000;
        case 44100:
            return MP3::Tables::ScaleFactorBandLong44100;
        case 48000:
            return MP3::Tables::ScaleFactorBandLong48000;
        }
    }
    VERIFY_NOT_REACHED();
}

}
