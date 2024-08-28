/*
 * Copyright (c) 2021, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FlacLoader.h"
#include "FlacTypes.h"
#include "GenericTypes.h"
#include "LoaderError.h"
#include "MultiChannel.h"
#include "Resampler.h"
#include "VorbisComment.h"
#include <AK/ByteString.h>
#include <AK/Debug.h>
#include <AK/DeprecatedFlyString.h>
#include <AK/FixedArray.h>
#include <AK/Format.h>
#include <AK/IntegralMath.h>
#include <AK/Math.h>
#include <AK/MemoryStream.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/StdLibExtras.h>
#include <AK/Try.h>
#include <AK/TypedTransfer.h>
#include <AK/UFixedBigInt.h>
#include <LibCore/File.h>
#include <LibCrypto/Checksum/ChecksumFunction.h>
#include <LibCrypto/Checksum/ChecksummingStream.h>

namespace Audio {

FlacLoaderPlugin::FlacLoaderPlugin(NonnullOwnPtr<SeekableStream> stream)
    : LoaderPlugin(move(stream))
{
}

ErrorOr<NonnullOwnPtr<LoaderPlugin>, LoaderError> FlacLoaderPlugin::create(NonnullOwnPtr<SeekableStream> stream)
{
    auto loader = make<FlacLoaderPlugin>(move(stream));
    TRY(loader->initialize());
    return loader;
}

MaybeLoaderError FlacLoaderPlugin::initialize()
{
    TRY(parse_header());
    TRY(reset());
    return {};
}

bool FlacLoaderPlugin::sniff(SeekableStream& stream)
{
    BigEndianInputBitStream bit_input { MaybeOwned<Stream>(stream) };
    auto maybe_flac = bit_input.read_bits<u32>(32);
    return !maybe_flac.is_error() && maybe_flac.value() == 0x664C6143; // "flaC"
}

// 11.5 STREAM
MaybeLoaderError FlacLoaderPlugin::parse_header()
{
    BigEndianInputBitStream bit_input { MaybeOwned<Stream>(*m_stream) };

    // A mixture of VERIFY and the non-crashing TRY().
#define FLAC_VERIFY(check, category, msg)                                                                          \
    do {                                                                                                           \
        if (!(check)) {                                                                                            \
            return LoaderError { category, TRY(m_stream->tell()), ByteString::formatted("FLAC header: {}", msg) }; \
        }                                                                                                          \
    } while (0)

    // Magic number
    u32 flac = TRY(bit_input.read_bits<u32>(32));
    m_data_start_location += 4;
    FLAC_VERIFY(flac == 0x664C6143, LoaderError::Category::Format, "Magic number must be 'flaC'"); // "flaC"

    // Receive the streaminfo block
    auto streaminfo = TRY(next_meta_block(bit_input));
    FLAC_VERIFY(streaminfo.type == FlacMetadataBlockType::STREAMINFO, LoaderError::Category::Format, "First block must be STREAMINFO");
    FixedMemoryStream streaminfo_data_memory { streaminfo.data.bytes() };
    BigEndianInputBitStream streaminfo_data { MaybeOwned<Stream>(streaminfo_data_memory) };

    // 11.10 METADATA_BLOCK_STREAMINFO
    m_min_block_size = TRY(streaminfo_data.read_bits<u16>(16));
    FLAC_VERIFY(m_min_block_size >= 16, LoaderError::Category::Format, "Minimum block size must be 16");
    m_max_block_size = TRY(streaminfo_data.read_bits<u16>(16));
    FLAC_VERIFY(m_max_block_size >= 16, LoaderError::Category::Format, "Maximum block size");
    m_min_frame_size = TRY(streaminfo_data.read_bits<u32>(24));
    m_max_frame_size = TRY(streaminfo_data.read_bits<u32>(24));
    m_sample_rate = TRY(streaminfo_data.read_bits<u32>(20));
    FLAC_VERIFY(m_sample_rate <= 655350, LoaderError::Category::Format, "Sample rate");
    m_num_channels = TRY(streaminfo_data.read_bits<u8>(3)) + 1; // 0 = one channel

    m_bits_per_sample = TRY(streaminfo_data.read_bits<u8>(5)) + 1;
    if (m_bits_per_sample <= 8) {
        // FIXME: Signed/Unsigned issues?
        m_sample_format = PcmSampleFormat::Uint8;
    } else if (m_bits_per_sample <= 16) {
        m_sample_format = PcmSampleFormat::Int16;
    } else if (m_bits_per_sample <= 24) {
        m_sample_format = PcmSampleFormat::Int24;
    } else if (m_bits_per_sample <= 32) {
        m_sample_format = PcmSampleFormat::Int32;
    } else {
        FLAC_VERIFY(false, LoaderError::Category::Format, "Sample bit depth too large");
    }

    m_total_samples = TRY(streaminfo_data.read_bits<u64>(36));
    if (m_total_samples == 0) {
        // "A value of zero here means the number of total samples is unknown."
        dbgln("FLAC Warning: File has unknown amount of samples, the loader will not stop before EOF");
        m_total_samples = NumericLimits<decltype(m_total_samples)>::max();
    }

    VERIFY(streaminfo_data.is_aligned_to_byte_boundary());
    TRY(streaminfo_data.read_until_filled({ m_md5_checksum, sizeof(m_md5_checksum) }));

    // Parse other blocks
    [[maybe_unused]] u16 meta_blocks_parsed = 1;
    [[maybe_unused]] u16 total_meta_blocks = meta_blocks_parsed;
    FlacRawMetadataBlock block = streaminfo;
    while (!block.is_last_block) {
        block = TRY(next_meta_block(bit_input));
        switch (block.type) {
        case (FlacMetadataBlockType::SEEKTABLE):
            TRY(load_seektable(block));
            break;
        case FlacMetadataBlockType::PICTURE:
            TRY(load_picture(block));
            break;
        case FlacMetadataBlockType::APPLICATION:
            // Note: Third-party library can encode specific data in this.
            dbgln("FLAC Warning: Unknown 'Application' metadata block encountered.");
            [[fallthrough]];
        case FlacMetadataBlockType::PADDING:
            // Note: A padding block is empty and does not need any treatment.
            break;
        case FlacMetadataBlockType::VORBIS_COMMENT:
            load_vorbis_comment(block);
            break;
        default:
            // TODO: Parse the remaining metadata block types.
            break;
        }
        ++total_meta_blocks;
    }

    dbgln_if(AFLACLOADER_DEBUG, "Parsed FLAC header: blocksize {}-{}{}, framesize {}-{}, {}Hz, {}bit, {} channels, {} samples total ({:.2f}s), MD5 {}, data start at {:x} bytes, {} headers total (skipped {})", m_min_block_size, m_max_block_size, is_fixed_blocksize_stream() ? " (constant)" : "", m_min_frame_size, m_max_frame_size, m_sample_rate, pcm_bits_per_sample(m_sample_format), m_num_channels, m_total_samples, static_cast<float>(m_total_samples) / static_cast<float>(m_sample_rate), m_md5_checksum, m_data_start_location, total_meta_blocks, total_meta_blocks - meta_blocks_parsed);
    TRY(m_seektable.insert_seek_point({ 0, 0 }));

    return {};
}

// 11.19. METADATA_BLOCK_PICTURE
MaybeLoaderError FlacLoaderPlugin::load_picture(FlacRawMetadataBlock& block)
{
    FixedMemoryStream memory_stream { block.data.bytes() };
    BigEndianInputBitStream picture_block_bytes { MaybeOwned<Stream>(memory_stream) };

    PictureData picture;

    picture.type = static_cast<ID3PictureType>(TRY(picture_block_bytes.read_bits(32)));

    auto const mime_string_length = TRY(picture_block_bytes.read_bits(32));
    auto offset_before_seeking = memory_stream.offset();
    if (offset_before_seeking + mime_string_length >= block.data.size())
        return LoaderError { LoaderError::Category::Format, TRY(m_stream->tell()), "Picture MIME type exceeds available data" };

    // "The MIME type string, in printable ASCII characters 0x20-0x7E."
    picture.mime_string = TRY(String::from_stream(memory_stream, mime_string_length));
    for (auto code_point : picture.mime_string.code_points()) {
        if (code_point < 0x20 || code_point > 0x7E)
            return LoaderError { LoaderError::Category::Format, TRY(m_stream->tell()), "Picture MIME type is not ASCII in range 0x20 - 0x7E" };
    }

    auto const description_string_length = TRY(picture_block_bytes.read_bits(32));
    offset_before_seeking = memory_stream.offset();
    if (offset_before_seeking + description_string_length >= block.data.size())
        return LoaderError { LoaderError::Category::Format, TRY(m_stream->tell()), "Picture description exceeds available data" };

    picture.description_string = TRY(String::from_stream(memory_stream, description_string_length));

    picture.width = TRY(picture_block_bytes.read_bits(32));
    picture.height = TRY(picture_block_bytes.read_bits(32));

    picture.color_depth = TRY(picture_block_bytes.read_bits(32));
    picture.colors = TRY(picture_block_bytes.read_bits(32));

    auto const picture_size = TRY(picture_block_bytes.read_bits(32));
    offset_before_seeking = memory_stream.offset();
    if (offset_before_seeking + picture_size > block.data.size())
        return LoaderError { LoaderError::Category::Format, static_cast<size_t>(TRY(m_stream->tell())), "Picture size exceeds available data" };

    TRY(memory_stream.seek(picture_size, SeekMode::FromCurrentPosition));
    picture.data = Vector<u8> { block.data.bytes().slice(offset_before_seeking, picture_size) };

    m_pictures.append(move(picture));

    return {};
}

// 11.15. METADATA_BLOCK_VORBIS_COMMENT
void FlacLoaderPlugin::load_vorbis_comment(FlacRawMetadataBlock& block)
{
    auto metadata_or_error = Audio::load_vorbis_comment(block.data);
    if (metadata_or_error.is_error()) {
        dbgln("FLAC Warning: Vorbis comment invalid, error: {}", metadata_or_error.release_error());
        return;
    }
    m_metadata = metadata_or_error.release_value();
}

// 11.13. METADATA_BLOCK_SEEKTABLE
MaybeLoaderError FlacLoaderPlugin::load_seektable(FlacRawMetadataBlock& block)
{
    FixedMemoryStream memory_stream { block.data.bytes() };
    BigEndianInputBitStream seektable_bytes { MaybeOwned<Stream>(memory_stream) };
    for (size_t i = 0; i < block.length / 18; ++i) {
        // 11.14. SEEKPOINT
        u64 sample_index = TRY(seektable_bytes.read_bits<u64>(64));
        u64 byte_offset = TRY(seektable_bytes.read_bits<u64>(64));
        // The sample count of a seek point is not relevant to us.
        [[maybe_unused]] u16 sample_count = TRY(seektable_bytes.read_bits<u16>(16));
        // Placeholder, to be ignored.
        if (sample_index == 0xFFFFFFFFFFFFFFFF)
            continue;

        SeekPoint seekpoint {
            .sample_index = sample_index,
            .byte_offset = byte_offset,
        };
        TRY(m_seektable.insert_seek_point(seekpoint));
    }
    dbgln_if(AFLACLOADER_DEBUG, "Loaded seektable of size {}", m_seektable.size());
    return {};
}

// 11.6 METADATA_BLOCK
ErrorOr<FlacRawMetadataBlock, LoaderError> FlacLoaderPlugin::next_meta_block(BigEndianInputBitStream& bit_input)
{
    // 11.7 METADATA_BLOCK_HEADER
    bool is_last_block = TRY(bit_input.read_bit());
    // The block type enum constants agree with the specification
    FlacMetadataBlockType type = (FlacMetadataBlockType)TRY(bit_input.read_bits<u8>(7));
    m_data_start_location += 1;
    FLAC_VERIFY(type != FlacMetadataBlockType::INVALID, LoaderError::Category::Format, "Invalid metadata block");

    u32 block_length = TRY(bit_input.read_bits<u32>(24));
    m_data_start_location += 3;
    // Blocks can be zero-sized, which would trip up the raw data reader below.
    if (block_length == 0)
        return FlacRawMetadataBlock {
            .is_last_block = is_last_block,
            .type = type,
            .length = 0,
            .data = TRY(ByteBuffer::create_uninitialized(0))
        };
    auto block_data_result = ByteBuffer::create_uninitialized(block_length);
    FLAC_VERIFY(!block_data_result.is_error(), LoaderError::Category::IO, "Out of memory");
    auto block_data = block_data_result.release_value();

    TRY(bit_input.read_until_filled(block_data));

    m_data_start_location += block_length;
    return FlacRawMetadataBlock {
        is_last_block,
        type,
        block_length,
        block_data,
    };
}
#undef FLAC_VERIFY

MaybeLoaderError FlacLoaderPlugin::reset()
{
    TRY(seek(0));
    m_current_frame.clear();
    return {};
}

MaybeLoaderError FlacLoaderPlugin::seek(int int_sample_index)
{
    auto sample_index = static_cast<size_t>(int_sample_index);
    if (sample_index == m_loaded_samples)
        return {};

    auto maybe_target_seekpoint = m_seektable.seek_point_before(sample_index);
    // No seektable or no fitting entry: Perform normal forward read
    if (!maybe_target_seekpoint.has_value()) {
        if (sample_index < m_loaded_samples) {
            TRY(m_stream->seek(m_data_start_location, SeekMode::SetPosition));
            m_loaded_samples = 0;
        }
        if (sample_index - m_loaded_samples == 0)
            return {};
        dbgln_if(AFLACLOADER_DEBUG, "Seeking {} samples manually", sample_index - m_loaded_samples);
    } else {
        auto target_seekpoint = maybe_target_seekpoint.release_value();

        // When a small seek happens, we may already be closer to the target than the seekpoint.
        if (sample_index - target_seekpoint.sample_index > sample_index - m_loaded_samples) {
            dbgln_if(AFLACLOADER_DEBUG, "Close enough to target ({} samples): ignoring seek point", sample_index - m_loaded_samples);
        } else {
            dbgln_if(AFLACLOADER_DEBUG, "Seeking to seektable: sample index {}, byte offset {}", target_seekpoint.sample_index, target_seekpoint.byte_offset);
            auto position = target_seekpoint.byte_offset + m_data_start_location;
            if (m_stream->seek(static_cast<i64>(position), SeekMode::SetPosition).is_error())
                return LoaderError { LoaderError::Category::IO, m_loaded_samples, ByteString::formatted("Invalid seek position {}", position) };
            m_loaded_samples = target_seekpoint.sample_index;
        }
    }

    // Skip frames until we're just before the target sample.
    VERIFY(m_loaded_samples <= sample_index);
    size_t frame_start_location;
    while (m_loaded_samples <= sample_index) {
        frame_start_location = TRY(m_stream->tell());
        (void)TRY(next_frame());
        m_loaded_samples += m_current_frame->sample_count;
    }
    TRY(m_stream->seek(frame_start_location, SeekMode::SetPosition));

    return {};
}

bool FlacLoaderPlugin::should_insert_seekpoint_at(u64 sample_index) const
{
    auto const max_seekpoint_distance = (maximum_seekpoint_distance_ms * m_sample_rate) / 1000;
    auto const seek_tolerance = (seek_tolerance_ms * m_sample_rate) / 1000;
    auto const current_seekpoint_distance = m_seektable.seek_point_sample_distance_around(sample_index).value_or(NumericLimits<u64>::max());
    auto const previous_seekpoint = m_seektable.seek_point_before(sample_index);
    auto const distance_to_previous_seekpoint = previous_seekpoint.has_value() ? sample_index - previous_seekpoint->sample_index : NumericLimits<u64>::max();

    // We insert a seekpoint only under two conditions:
    // - The seek points around us are spaced too far for what the loader recommends.
    //   Prevents inserting too many seek points between pre-loaded seek points.
    // - We are so far away from the previous seek point that seeking will become too imprecise if we don't insert a seek point at least here.
    //   Prevents inserting too many seek points at the end of files without pre-loaded seek points.
    return current_seekpoint_distance >= max_seekpoint_distance && distance_to_previous_seekpoint >= seek_tolerance;
}

ErrorOr<Vector<FixedArray<Sample>>, LoaderError> FlacLoaderPlugin::load_chunks(size_t samples_to_read_from_input)
{
    ssize_t remaining_samples = static_cast<ssize_t>(m_total_samples - m_loaded_samples);
    // The first condition is relevant for unknown-size streams (total samples = 0 in the header)
    if (m_stream->is_eof() || (m_total_samples < NumericLimits<u64>::max() && remaining_samples <= 0))
        return Vector<FixedArray<Sample>> {};

    size_t samples_to_read = min(samples_to_read_from_input, remaining_samples);
    Vector<FixedArray<Sample>> frames;
    // In this case we can know exactly how many frames we're going to read.
    if (is_fixed_blocksize_stream() && m_current_frame.has_value())
        TRY(frames.try_ensure_capacity(samples_to_read / m_current_frame->sample_count + 1));

    size_t sample_index = 0;

    while (!m_stream->is_eof() && sample_index < samples_to_read) {
        TRY(frames.try_append(TRY(next_frame())));
        sample_index += m_current_frame->sample_count;
    }

    m_loaded_samples += sample_index;

    return frames;
}

// 11.21. FRAME
LoaderSamples FlacLoaderPlugin::next_frame()
{
#define FLAC_VERIFY(check, category, msg)                                                                                                   \
    do {                                                                                                                                    \
        if (!(check)) {                                                                                                                     \
            return LoaderError { category, static_cast<size_t>(m_current_sample_or_frame), ByteString::formatted("FLAC header: {}", msg) }; \
        }                                                                                                                                   \
    } while (0)

    auto frame_byte_index = TRY(m_stream->tell());
    auto sample_index = m_loaded_samples;
    // Insert a new seek point if we don't have enough here.
    if (should_insert_seekpoint_at(sample_index)) {
        dbgln_if(AFLACLOADER_DEBUG, "Inserting ad-hoc seek point for sample {} at byte {:x} (seekpoint spacing {} samples)", sample_index, frame_byte_index, m_seektable.seek_point_sample_distance_around(sample_index).value_or(NumericLimits<u64>::max()));
        auto maybe_error = m_seektable.insert_seek_point({ .sample_index = sample_index, .byte_offset = frame_byte_index - m_data_start_location });
        if (maybe_error.is_error())
            dbgln("FLAC Warning: Inserting seek point for sample {} failed: {}", sample_index, maybe_error.release_error());
    }

    auto frame_checksum_stream = TRY(try_make<Crypto::Checksum::ChecksummingStream<IBMCRC>>(MaybeOwned<Stream>(*m_stream)));
    auto header_checksum_stream = TRY(try_make<Crypto::Checksum::ChecksummingStream<FlacFrameHeaderCRC>>(MaybeOwned<Stream>(*frame_checksum_stream)));
    BigEndianInputBitStream bit_stream { MaybeOwned<Stream> { *header_checksum_stream } };

    // 11.22. FRAME_HEADER
    u16 sync_code = TRY(bit_stream.read_bits<u16>(14));
    FLAC_VERIFY(sync_code == 0b11111111111110, LoaderError::Category::Format, "Sync code");
    bool reserved_bit = TRY(bit_stream.read_bit());
    FLAC_VERIFY(reserved_bit == 0, LoaderError::Category::Format, "Reserved frame header bit");
    // 11.22.2. BLOCKING STRATEGY
    [[maybe_unused]] bool blocking_strategy = TRY(bit_stream.read_bit());

    u32 sample_count = TRY(convert_sample_count_code(TRY(bit_stream.read_bits<u8>(4))));

    u32 frame_sample_rate = TRY(convert_sample_rate_code(TRY(bit_stream.read_bits<u8>(4))));

    u8 channel_type_num = TRY(bit_stream.read_bits<u8>(4));
    FLAC_VERIFY(channel_type_num < 0b1011, LoaderError::Category::Format, "Channel assignment");
    FlacFrameChannelType channel_type = (FlacFrameChannelType)channel_type_num;

    u8 bit_depth = TRY(convert_bit_depth_code(TRY(bit_stream.read_bits<u8>(3))));

    reserved_bit = TRY(bit_stream.read_bit());
    FLAC_VERIFY(reserved_bit == 0, LoaderError::Category::Format, "Reserved frame header end bit");

    // 11.22.8. CODED NUMBER
    m_current_sample_or_frame = TRY(read_utf8_char(bit_stream));

    // Conditional header variables
    // 11.22.9. BLOCK SIZE INT
    if (sample_count == FLAC_BLOCKSIZE_AT_END_OF_HEADER_8) {
        sample_count = TRY(bit_stream.read_bits<u32>(8)) + 1;
    } else if (sample_count == FLAC_BLOCKSIZE_AT_END_OF_HEADER_16) {
        sample_count = TRY(bit_stream.read_bits<u32>(16)) + 1;
    }

    // 11.22.10. SAMPLE RATE INT
    if (frame_sample_rate == FLAC_SAMPLERATE_AT_END_OF_HEADER_8) {
        frame_sample_rate = TRY(bit_stream.read_bits<u32>(8)) * 1000;
    } else if (frame_sample_rate == FLAC_SAMPLERATE_AT_END_OF_HEADER_16) {
        frame_sample_rate = TRY(bit_stream.read_bits<u32>(16));
    } else if (frame_sample_rate == FLAC_SAMPLERATE_AT_END_OF_HEADER_16X10) {
        frame_sample_rate = TRY(bit_stream.read_bits<u32>(16)) * 10;
    }

    // It does not matter whether we extract the checksum from the digest here, or extract the digest 0x00 after processing the checksum.
    auto const calculated_header_checksum = header_checksum_stream->digest();
    // 11.22.11. FRAME CRC
    u8 specified_header_checksum = TRY(bit_stream.read_bits<u8>(8));
    VERIFY(bit_stream.is_aligned_to_byte_boundary());
    if (specified_header_checksum != calculated_header_checksum)
        dbgln("FLAC frame {}: Calculated header checksum {:02x} is different from specified checksum {:02x}", m_current_sample_or_frame, calculated_header_checksum, specified_header_checksum);

    dbgln_if(AFLACLOADER_DEBUG, "Frame: {} samples, {}bit {}Hz, channeltype {:x}, {} number {}, header checksum {:02x}{}", sample_count, bit_depth, frame_sample_rate, channel_type_num, blocking_strategy ? "sample" : "frame", m_current_sample_or_frame, specified_header_checksum, specified_header_checksum != calculated_header_checksum ? " (checksum error)"sv : ""sv);

    m_current_frame = FlacFrameHeader {
        .sample_rate = frame_sample_rate,
        .sample_count = static_cast<u16>(sample_count),
        .sample_or_frame_index = static_cast<u32>(m_current_sample_or_frame),
        .blocking_strategy = static_cast<BlockingStrategy>(blocking_strategy),
        .channels = channel_type,
        .bit_depth = bit_depth,
        .checksum = specified_header_checksum,
    };

    u8 subframe_count = frame_channel_type_to_channel_count(channel_type);
    TRY(m_subframe_buffers.try_resize_and_keep_capacity(subframe_count));

    float sample_rescale = 1 / static_cast<float>(1 << (m_current_frame->bit_depth - 1));
    dbgln_if(AFLACLOADER_DEBUG, "Samples will be rescaled from {} bits: factor {:.8f}", m_current_frame->bit_depth, sample_rescale);

    for (u8 i = 0; i < subframe_count; ++i) {
        FlacSubframeHeader new_subframe = TRY(next_subframe_header(bit_stream, i));
        auto& subframe_samples = m_subframe_buffers[i];
        subframe_samples.clear_with_capacity();
        TRY(parse_subframe(subframe_samples, new_subframe, bit_stream));
        // We only verify the sample count for the common case of a constant sample rate.
        if (m_sample_rate == m_current_frame->sample_rate)
            VERIFY(subframe_samples.size() == m_current_frame->sample_count);
    }

    // 11.2. Overview ("The audio data is composed of...")
    bit_stream.align_to_byte_boundary();

    // 11.23. FRAME_FOOTER
    auto const calculated_frame_checksum = frame_checksum_stream->digest();
    auto const specified_frame_checksum = TRY(bit_stream.read_bits<u16>(16));
    if (calculated_frame_checksum != specified_frame_checksum)
        dbgln("FLAC frame {}: Calculated frame checksum {:04x} is different from specified checksum {:04x}", m_current_sample_or_frame, calculated_frame_checksum, specified_frame_checksum);
    dbgln_if(AFLACLOADER_DEBUG, "Subframe footer checksum: {:04x}{}", specified_frame_checksum, specified_frame_checksum != calculated_frame_checksum ? " (checksum error)"sv : ""sv);

    FixedArray<Sample> samples;

    switch (channel_type) {
    case FlacFrameChannelType::Mono:
    case FlacFrameChannelType::Stereo:
    case FlacFrameChannelType::StereoCenter:
    case FlacFrameChannelType::Surround4p0:
    case FlacFrameChannelType::Surround5p0:
    case FlacFrameChannelType::Surround5p1:
    case FlacFrameChannelType::Surround6p1:
    case FlacFrameChannelType::Surround7p1: {
        auto new_samples = TRY(downmix_surround_to_stereo<Vector<i64>>(m_subframe_buffers, sample_rescale));
        samples.swap(new_samples);
        break;
    }
    case FlacFrameChannelType::LeftSideStereo: {
        auto new_samples = TRY(FixedArray<Sample>::create(m_current_frame->sample_count));
        samples.swap(new_samples);
        // channels are left (0) and side (1)
        for (size_t i = 0; i < m_current_frame->sample_count; ++i) {
            // right = left - side
            samples[i] = { static_cast<float>(m_subframe_buffers[0][i]) * sample_rescale,
                static_cast<float>(m_subframe_buffers[0][i] - m_subframe_buffers[1][i]) * sample_rescale };
        }
        break;
    }
    case FlacFrameChannelType::RightSideStereo: {
        auto new_samples = TRY(FixedArray<Sample>::create(m_current_frame->sample_count));
        samples.swap(new_samples);
        // channels are side (0) and right (1)
        for (size_t i = 0; i < m_current_frame->sample_count; ++i) {
            // left = right + side
            samples[i] = { static_cast<float>(m_subframe_buffers[1][i] + m_subframe_buffers[0][i]) * sample_rescale,
                static_cast<float>(m_subframe_buffers[1][i]) * sample_rescale };
        }
        break;
    }
    case FlacFrameChannelType::MidSideStereo: {
        auto new_samples = TRY(FixedArray<Sample>::create(m_current_frame->sample_count));
        samples.swap(new_samples);
        // channels are mid (0) and side (1)
        for (size_t i = 0; i < m_subframe_buffers[0].size(); ++i) {
            i64 mid = m_subframe_buffers[0][i];
            i64 side = m_subframe_buffers[1][i];
            mid *= 2;
            // prevent integer division errors
            samples[i] = { static_cast<float>(mid + side) * .5f * sample_rescale,
                static_cast<float>(mid - side) * .5f * sample_rescale };
        }
        break;
    }
    }

    return samples;
#undef FLAC_VERIFY
}

// 11.22.3. INTERCHANNEL SAMPLE BLOCK SIZE
ErrorOr<u32, LoaderError> FlacLoaderPlugin::convert_sample_count_code(u8 sample_count_code)
{
    // single codes
    switch (sample_count_code) {
    case 0:
        return LoaderError { LoaderError::Category::Format, static_cast<size_t>(m_current_sample_or_frame), "Reserved block size" };
    case 1:
        return 192;
    case 6:
        return FLAC_BLOCKSIZE_AT_END_OF_HEADER_8;
    case 7:
        return FLAC_BLOCKSIZE_AT_END_OF_HEADER_16;
    }
    if (sample_count_code >= 2 && sample_count_code <= 5) {
        return 576 * AK::exp2(sample_count_code - 2);
    }
    return 256 * AK::exp2(sample_count_code - 8);
}

// 11.22.4. SAMPLE RATE
ErrorOr<u32, LoaderError> FlacLoaderPlugin::convert_sample_rate_code(u8 sample_rate_code)
{
    switch (sample_rate_code) {
    case 0:
        return m_sample_rate;
    case 1:
        return 88200;
    case 2:
        return 176400;
    case 3:
        return 192000;
    case 4:
        return 8000;
    case 5:
        return 16000;
    case 6:
        return 22050;
    case 7:
        return 24000;
    case 8:
        return 32000;
    case 9:
        return 44100;
    case 10:
        return 48000;
    case 11:
        return 96000;
    case 12:
        return FLAC_SAMPLERATE_AT_END_OF_HEADER_8;
    case 13:
        return FLAC_SAMPLERATE_AT_END_OF_HEADER_16;
    case 14:
        return FLAC_SAMPLERATE_AT_END_OF_HEADER_16X10;
    default:
        return LoaderError { LoaderError::Category::Format, static_cast<size_t>(m_current_sample_or_frame), "Invalid sample rate code" };
    }
}

// 11.22.6. SAMPLE SIZE
ErrorOr<u8, LoaderError> FlacLoaderPlugin::convert_bit_depth_code(u8 bit_depth_code)
{
    switch (bit_depth_code) {
    case 0:
        return m_bits_per_sample;
    case 1:
        return 8;
    case 2:
        return 12;
    case 3:
        return LoaderError { LoaderError::Category::Format, static_cast<size_t>(m_current_sample_or_frame), "Reserved sample size" };
    case 4:
        return 16;
    case 5:
        return 20;
    case 6:
        return 24;
    case 7:
        return 32;
    default:
        return LoaderError { LoaderError::Category::Format, static_cast<size_t>(m_current_sample_or_frame), ByteString::formatted("Unsupported sample size {}", bit_depth_code) };
    }
}

// 11.22.5. CHANNEL ASSIGNMENT
u8 frame_channel_type_to_channel_count(FlacFrameChannelType channel_type)
{
    if (channel_type <= FlacFrameChannelType::Surround7p1)
        return to_underlying(channel_type) + 1;
    return 2;
}

// 11.25. SUBFRAME_HEADER
ErrorOr<FlacSubframeHeader, LoaderError> FlacLoaderPlugin::next_subframe_header(BigEndianInputBitStream& bit_stream, u8 channel_index)
{
    u8 bits_per_sample = m_current_frame->bit_depth;

    // For inter-channel correlation, the side channel needs an extra bit for its samples
    switch (m_current_frame->channels) {
    case FlacFrameChannelType::LeftSideStereo:
    case FlacFrameChannelType::MidSideStereo:
        if (channel_index == 1) {
            ++bits_per_sample;
        }
        break;
    case FlacFrameChannelType::RightSideStereo:
        if (channel_index == 0) {
            ++bits_per_sample;
        }
        break;
    // "normal" channel types
    default:
        break;
    }

    // zero-bit padding
    if (TRY(bit_stream.read_bit()) != 0)
        return LoaderError { LoaderError::Category::Format, static_cast<size_t>(m_current_sample_or_frame), "Zero bit padding" };

    // 11.25.1. SUBFRAME TYPE
    u8 subframe_code = TRY(bit_stream.read_bits<u8>(6));
    if ((subframe_code >= 0b000010 && subframe_code <= 0b000111) || (subframe_code > 0b001100 && subframe_code < 0b100000))
        return LoaderError { LoaderError::Category::Format, static_cast<size_t>(m_current_sample_or_frame), "Subframe type" };

    FlacSubframeType subframe_type;
    u8 order = 0;
    // LPC has the highest bit set
    if ((subframe_code & 0b100000) > 0) {
        subframe_type = FlacSubframeType::LPC;
        order = (subframe_code & 0b011111) + 1;
    } else if ((subframe_code & 0b001000) > 0) {
        // Fixed has the third-highest bit set
        subframe_type = FlacSubframeType::Fixed;
        order = (subframe_code & 0b000111);
    } else {
        subframe_type = (FlacSubframeType)subframe_code;
    }

    // 11.25.2. WASTED BITS PER SAMPLE FLAG
    bool has_wasted_bits = TRY(bit_stream.read_bit());
    u8 k = 0;
    if (has_wasted_bits) {
        bool current_k_bit = 0;
        do {
            current_k_bit = TRY(bit_stream.read_bit());
            ++k;
        } while (current_k_bit != 1);
    }

    return FlacSubframeHeader {
        subframe_type,
        order,
        k,
        bits_per_sample
    };
}

ErrorOr<void, LoaderError> FlacLoaderPlugin::parse_subframe(Vector<i64>& samples, FlacSubframeHeader& subframe_header, BigEndianInputBitStream& bit_input)
{
    TRY(samples.try_ensure_capacity(m_current_frame->sample_count));

    switch (subframe_header.type) {
    case FlacSubframeType::Constant: {
        // 11.26. SUBFRAME_CONSTANT
        u64 constant_value = TRY(bit_input.read_bits<u64>(subframe_header.bits_per_sample - subframe_header.wasted_bits_per_sample));
        dbgln_if(AFLACLOADER_DEBUG, "  Constant subframe: {}", constant_value);

        VERIFY(subframe_header.bits_per_sample - subframe_header.wasted_bits_per_sample != 0);
        i64 constant = sign_extend(static_cast<u64>(constant_value), subframe_header.bits_per_sample - subframe_header.wasted_bits_per_sample);
        for (u64 i = 0; i < m_current_frame->sample_count; ++i) {
            samples.unchecked_append(constant);
        }
        break;
    }
    case FlacSubframeType::Fixed: {
        dbgln_if(AFLACLOADER_DEBUG, "  Fixed LPC subframe order {}", subframe_header.order);
        samples = TRY(decode_fixed_lpc(subframe_header, bit_input));
        break;
    }
    case FlacSubframeType::Verbatim: {
        dbgln_if(AFLACLOADER_DEBUG, "  Verbatim subframe");
        samples = TRY(decode_verbatim(subframe_header, bit_input));
        break;
    }
    case FlacSubframeType::LPC: {
        dbgln_if(AFLACLOADER_DEBUG, "  Custom LPC subframe order {}", subframe_header.order);
        TRY(decode_custom_lpc(samples, subframe_header, bit_input));
        break;
    }
    default:
        return LoaderError { LoaderError::Category::Unimplemented, static_cast<size_t>(m_current_sample_or_frame), "Unhandled FLAC subframe type" };
    }

    for (size_t i = 0; i < samples.size(); ++i) {
        samples[i] <<= subframe_header.wasted_bits_per_sample;
    }

    // Resamplers VERIFY that the sample rate is non-zero.
    if (m_current_frame->sample_rate == 0 || m_sample_rate == 0
        || m_current_frame->sample_rate == m_sample_rate)
        return {};

    ResampleHelper<i64> resampler(m_current_frame->sample_rate, m_sample_rate);
    samples = resampler.resample(samples);
    return {};
}

// 11.29. SUBFRAME_VERBATIM
// Decode a subframe that isn't actually encoded, usually seen in random data
ErrorOr<Vector<i64>, LoaderError> FlacLoaderPlugin::decode_verbatim(FlacSubframeHeader& subframe, BigEndianInputBitStream& bit_input)
{
    Vector<i64> decoded;
    decoded.ensure_capacity(m_current_frame->sample_count);

    if (subframe.bits_per_sample <= subframe.wasted_bits_per_sample) {
        return LoaderError {
            LoaderError::Category::Format,
            TRY(m_stream->tell()),
            "Effective verbatim bits per sample are zero"sv,
        };
    }
    for (size_t i = 0; i < m_current_frame->sample_count; ++i) {
        decoded.unchecked_append(sign_extend(
            TRY(bit_input.read_bits<u64>(subframe.bits_per_sample - subframe.wasted_bits_per_sample)),
            subframe.bits_per_sample - subframe.wasted_bits_per_sample));
    }

    return decoded;
}

// 11.28. SUBFRAME_LPC
// Decode a subframe encoded with a custom linear predictor coding, i.e. the subframe provides the polynomial order and coefficients
ErrorOr<void, LoaderError> FlacLoaderPlugin::decode_custom_lpc(Vector<i64>& decoded, FlacSubframeHeader& subframe, BigEndianInputBitStream& bit_input)
{
    // LPC must provide at least as many samples as its order.
    if (subframe.order > m_current_frame->sample_count)
        return LoaderError { LoaderError::Category::Format, static_cast<size_t>(m_current_sample_or_frame), "Too small frame for LPC order" };

    decoded.ensure_capacity(m_current_frame->sample_count);

    if (subframe.bits_per_sample <= subframe.wasted_bits_per_sample) {
        return LoaderError {
            LoaderError::Category::Format,
            TRY(m_stream->tell()),
            "Effective verbatim bits per sample are zero"sv,
        };
    }
    // warm-up samples
    for (auto i = 0; i < subframe.order; ++i) {
        decoded.unchecked_append(sign_extend(
            TRY(bit_input.read_bits<u64>(subframe.bits_per_sample - subframe.wasted_bits_per_sample)),
            subframe.bits_per_sample - subframe.wasted_bits_per_sample));
    }

    // precision of the coefficients
    u8 lpc_precision = TRY(bit_input.read_bits<u8>(4));
    if (lpc_precision == 0b1111)
        return LoaderError { LoaderError::Category::Format, static_cast<size_t>(m_current_sample_or_frame), "Invalid linear predictor coefficient precision" };
    lpc_precision += 1;

    // shift needed on the data (signed!)
    i8 lpc_shift = static_cast<i8>(sign_extend(TRY(bit_input.read_bits<u8>(5)), 5));

    Vector<i64, 32> coefficients;
    coefficients.ensure_capacity(subframe.order);
    // read coefficients
    for (auto i = 0; i < subframe.order; ++i) {
        u64 raw_coefficient = TRY(bit_input.read_bits<u64>(lpc_precision));
        i64 coefficient = sign_extend(raw_coefficient, lpc_precision);
        coefficients.unchecked_append(coefficient);
    }

    dbgln_if(AFLACLOADER_DEBUG, "    {}-bit {} shift coefficients: {}", lpc_precision, lpc_shift, coefficients);

    TRY(decode_residual(decoded, subframe, bit_input));

    // approximate the waveform with the predictor
    for (size_t i = subframe.order; i < m_current_frame->sample_count; ++i) {
        // (see below)
        Checked<i64> sample = 0;
        for (size_t t = 0; t < subframe.order; ++t) {
            // It's really important that we compute in 64-bit land here.
            // Even though FLAC operates at a maximum bit depth of 32 bits, modern encoders use super-large coefficients for maximum compression.
            // These will easily overflow 32 bits and cause strange white noise that abruptly stops intermittently (at the end of a frame).
            // The simple fix of course is to do intermediate computations in 64 bits, but we additionally use saturating arithmetic.
            // These considerations are not in the original FLAC spec, but have been added to the IETF standard: https://datatracker.ietf.org/doc/html/draft-ietf-cellar-flac-03#appendix-A.3
            sample.saturating_add(Checked<i64>::saturating_mul(static_cast<i64>(coefficients[t]), static_cast<i64>(decoded[i - t - 1])));
        }
        decoded[i] += lpc_shift >= 0 ? (sample.value() >> lpc_shift) : (sample.value() << -lpc_shift);
    }

    return {};
}

// 11.27. SUBFRAME_FIXED
// Decode a subframe encoded with one of the fixed linear predictor codings
ErrorOr<Vector<i64>, LoaderError> FlacLoaderPlugin::decode_fixed_lpc(FlacSubframeHeader& subframe, BigEndianInputBitStream& bit_input)
{
    // LPC must provide at least as many samples as its order.
    if (subframe.order > m_current_frame->sample_count)
        return LoaderError { LoaderError::Category::Format, static_cast<size_t>(m_current_sample_or_frame), "Too small frame for LPC order" };

    Vector<i64> decoded;
    decoded.ensure_capacity(m_current_frame->sample_count);

    if (subframe.bits_per_sample <= subframe.wasted_bits_per_sample) {
        return LoaderError {
            LoaderError::Category::Format,
            TRY(m_stream->tell()),
            "Effective verbatim bits per sample are zero"sv,
        };
    }
    // warm-up samples
    for (auto i = 0; i < subframe.order; ++i) {
        decoded.unchecked_append(sign_extend(
            TRY(bit_input.read_bits<u64>(subframe.bits_per_sample - subframe.wasted_bits_per_sample)),
            subframe.bits_per_sample - subframe.wasted_bits_per_sample));
    }

    TRY(decode_residual(decoded, subframe, bit_input));

    dbgln_if(AFLACLOADER_DEBUG, "    decoded length {}, {} order predictor, now at file offset {:x}", decoded.size(), subframe.order, TRY(m_stream->tell()));

    // Skip these comments if you don't care about the neat math behind fixed LPC :^)
    // These coefficients for the recursive prediction formula are the only ones that can be resolved to polynomial predictor functions.
    // The order equals the degree of the polynomial - 1, so the second-order predictor has an underlying polynomial of degree 1, a straight line.
    // More specifically, the closest approximation to a polynomial is used, and the degree depends on how many previous values are available.
    // This makes use of a very neat property of polynomials, which is that they are entirely characterized by their finitely many derivatives.
    // (Mathematically speaking, the infinite Taylor series of any polynomial equals the polynomial itself.)
    // Now remember that derivation is just the slope of the function, which is the same as the difference of two close-by values.
    // Therefore, with two samples we can calculate the first derivative at a sample via the difference, which gives us a polynomial of degree 1.
    // With three samples, we can do the same but also calculate the second derivative via the difference in the first derivatives.
    // This gives us a polynomial of degree 2, as it has two "proper" (non-constant) derivatives.
    // This can be continued for higher-order derivatives when we have more coefficients, giving us higher-order polynomials.
    // In essence, it's akin to a Lagrangian polynomial interpolation for every sample (but already pre-solved).

    // The coefficients for orders 0-3 originate from the SHORTEN codec:
    // http://mi.eng.cam.ac.uk/reports/svr-ftp/auto-pdf/robinson_tr156.pdf page 4
    // The coefficients for order 4 are undocumented in the original FLAC specification(s), but can now be found in
    // https://datatracker.ietf.org/doc/html/draft-ietf-cellar-flac-03#section-10.2.5
    // FIXME: Share this code with predict_fixed_lpc().
    switch (subframe.order) {
    case 0:
        // s_0(t) = 0
        for (u32 i = subframe.order; i < m_current_frame->sample_count; ++i)
            decoded[i] += 0;
        break;
    case 1:
        // s_1(t) = s(t-1)
        for (u32 i = subframe.order; i < m_current_frame->sample_count; ++i)
            decoded[i] += decoded[i - 1];
        break;
    case 2:
        // s_2(t) = 2s(t-1) - s(t-2)
        for (u32 i = subframe.order; i < m_current_frame->sample_count; ++i)
            decoded[i] += 2 * decoded[i - 1] - decoded[i - 2];
        break;
    case 3:
        // s_3(t) = 3s(t-1) - 3s(t-2) + s(t-3)
        for (u32 i = subframe.order; i < m_current_frame->sample_count; ++i)
            decoded[i] += 3 * decoded[i - 1] - 3 * decoded[i - 2] + decoded[i - 3];
        break;
    case 4:
        // s_4(t) = 4s(t-1) - 6s(t-2) + 4s(t-3) - s(t-4)
        for (u32 i = subframe.order; i < m_current_frame->sample_count; ++i)
            decoded[i] += 4 * decoded[i - 1] - 6 * decoded[i - 2] + 4 * decoded[i - 3] - decoded[i - 4];
        break;
    default:
        return LoaderError { LoaderError::Category::Format, static_cast<size_t>(m_current_sample_or_frame), ByteString::formatted("Unrecognized predictor order {}", subframe.order) };
    }
    return decoded;
}

// 11.30. RESIDUAL
// Decode the residual, the "error" between the function approximation and the actual audio data
MaybeLoaderError FlacLoaderPlugin::decode_residual(Vector<i64>& decoded, FlacSubframeHeader& subframe, BigEndianInputBitStream& bit_input)
{
    // 11.30.1. RESIDUAL_CODING_METHOD
    auto residual_mode = static_cast<FlacResidualMode>(TRY(bit_input.read_bits<u8>(2)));
    u8 partition_order = TRY(bit_input.read_bits<u8>(4));
    size_t partitions = 1 << partition_order;

    dbgln_if(AFLACLOADER_DEBUG, "    {}-bit Rice partitions, {} total (order {})", residual_mode == FlacResidualMode::Rice4Bit ? "4"sv : "5"sv, partitions, partition_order);

    if (partitions > m_current_frame->sample_count)
        return LoaderError { LoaderError::Category::Format, static_cast<size_t>(m_current_sample_or_frame), "Too many Rice partitions, each partition must contain at least one sample" };
    // “The partition order MUST be such that the block size is evenly divisible by the number of partitions.”
    // FIXME: Check “The partition order also MUST be such that the (block size >> partition order) is larger than the predictor order.”
    if (m_current_frame->sample_count % partitions != 0)
        return LoaderError { LoaderError::Category::Format, TRY(m_stream->tell()), "Block size is not evenly divisible by number of partitions" };

    if (residual_mode == FlacResidualMode::Rice4Bit) {
        // 11.30.2. RESIDUAL_CODING_METHOD_PARTITIONED_EXP_GOLOMB
        // decode a single Rice partition with four bits for the order k
        for (size_t i = 0; i < partitions; ++i) {
            // FIXME: Write into the decode buffer directly.
            auto rice_partition = TRY(decode_rice_partition(4, partitions, i, subframe, bit_input));
            decoded.extend(move(rice_partition));
        }
    } else if (residual_mode == FlacResidualMode::Rice5Bit) {
        // 11.30.3. RESIDUAL_CODING_METHOD_PARTITIONED_EXP_GOLOMB2
        // five bits equivalent
        for (size_t i = 0; i < partitions; ++i) {
            // FIXME: Write into the decode buffer directly.
            auto rice_partition = TRY(decode_rice_partition(5, partitions, i, subframe, bit_input));
            decoded.extend(move(rice_partition));
        }
    } else
        return LoaderError { LoaderError::Category::Format, static_cast<size_t>(m_current_sample_or_frame), "Reserved residual coding method" };

    return {};
}

// 11.30.2.1. EXP_GOLOMB_PARTITION and 11.30.3.1. EXP_GOLOMB2_PARTITION
// Decode a single Rice partition as part of the residual, every partition can have its own Rice parameter k
ALWAYS_INLINE ErrorOr<Vector<i64>, LoaderError> FlacLoaderPlugin::decode_rice_partition(u8 partition_type, u32 partitions, u32 partition_index, FlacSubframeHeader& subframe, BigEndianInputBitStream& bit_input)
{
    // 11.30.2.2. EXP GOLOMB PARTITION ENCODING PARAMETER and 11.30.3.2. EXP-GOLOMB2 PARTITION ENCODING PARAMETER
    u8 k = TRY(bit_input.read_bits<u8>(partition_type));

    u32 residual_sample_count;
    if (partitions == 0)
        residual_sample_count = m_current_frame->sample_count - subframe.order;
    else
        residual_sample_count = m_current_frame->sample_count / partitions;
    if (partition_index == 0) {
        if (subframe.order > residual_sample_count)
            return LoaderError { LoaderError::Category::Format, static_cast<size_t>(m_current_sample_or_frame), "First Rice partition must advertise more residuals than LPC order" };
        residual_sample_count -= subframe.order;
    }

    Vector<i64> rice_partition;
    rice_partition.resize(residual_sample_count);

    // escape code for unencoded binary partition
    if (k == (1 << partition_type) - 1) {
        u8 unencoded_bps = TRY(bit_input.read_bits<u8>(5));
        if (unencoded_bps != 0) {
            for (size_t r = 0; r < residual_sample_count; ++r) {
                rice_partition[r] = sign_extend(TRY(bit_input.read_bits<u32>(unencoded_bps)), unencoded_bps);
            }
        }
    } else {
        for (size_t r = 0; r < residual_sample_count; ++r) {
            rice_partition[r] = TRY(decode_unsigned_exp_golomb(k, bit_input));
        }
    }

    return rice_partition;
}

// Decode a single number encoded with Rice/Exponential-Golomb encoding (the unsigned variant)
ALWAYS_INLINE ErrorOr<i32> decode_unsigned_exp_golomb(u8 k, BigEndianInputBitStream& bit_input)
{
    u8 q = 0;
    while (TRY(bit_input.read_bit()) == 0)
        ++q;

    // least significant bits (remainder)
    u32 rem = TRY(bit_input.read_bits<u32>(k));
    u32 value = q << k | rem;

    return rice_to_signed(value);
}

ErrorOr<u64> read_utf8_char(BigEndianInputBitStream& input)
{
    u64 character;
    u8 start_byte = TRY(input.read_value<u8>());
    // Signal byte is zero: ASCII character
    if ((start_byte & 0b10000000) == 0) {
        return start_byte;
    } else if ((start_byte & 0b11000000) == 0b10000000) {
        return Error::from_string_literal("Illegal continuation byte");
    }
    // This algorithm supports the theoretical max 0xFF start byte, which is not part of the regular UTF-8 spec.
    u8 length = 1;
    while (((start_byte << length) & 0b10000000) == 0b10000000)
        ++length;

    // This is technically not spec-compliant, but if we take UTF-8 to its logical extreme,
    // we can say 0xFF means there's 7 following continuation bytes and no data at all in the leading character.
    if (length == 8) [[unlikely]] {
        character = 0;
    } else {
        u8 bits_from_start_byte = 8 - (length + 1);
        u8 start_byte_bitmask = AK::exp2(bits_from_start_byte) - 1;
        character = start_byte_bitmask & start_byte;
    }
    for (u8 i = length - 1; i > 0; --i) {
        u8 current_byte = TRY(input.read_value<u8>());
        character = (character << 6) | (current_byte & 0b00111111);
    }
    return character;
}

i64 sign_extend(u32 n, u8 size)
{
    // negative
    if ((n & (1 << (size - 1))) > 0) {
        return static_cast<i64>(n | (0xffffffffffffffffLL << size));
    }
    // positive
    return n;
}

i32 rice_to_signed(u32 x)
{
    // positive numbers are even, negative numbers are odd
    // bitmask for conditionally inverting the entire number, thereby "negating" it
    i32 sign = -static_cast<i32>(x & 1);
    // copies the sign's sign onto the actual magnitude of x
    return static_cast<i32>(sign ^ (x >> 1));
}
}
