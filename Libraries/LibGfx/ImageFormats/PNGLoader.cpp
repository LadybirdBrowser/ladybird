/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BitCast.h>
#include <AK/ByteBuffer.h>
#include <AK/Endian.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <LibGfx/ImageFormats/ExifOrientedBitmap.h>
#include <LibGfx/ImageFormats/PNGLoader.h>
#include <LibGfx/ImageFormats/TIFFLoader.h>
#include <LibGfx/ImageFormats/TIFFMetadata.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/Painter.h>
#include <png.h>
#include <zlib.h>

namespace Gfx {

struct PNGLoadingContext {
    ~PNGLoadingContext()
    {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    }

    png_structp png_ptr { nullptr };
    png_infop info_ptr { nullptr };

    ReadonlyBytes data;
    // If the input PNG had a raw-deflate IDAT, we rewrap it here into a valid zlib stream (see
    // maybe_rewrap_raw_deflate_idat) and retarget `data` at this buffer. Kept alive as long as the context is alive.
    ByteBuffer rewrapped_data;
    IntSize size;
    u32 frame_count { 0 };
    u32 loop_count { 0 };
    Vector<ImageFrameDescriptor> frame_descriptors;
    Optional<Media::CodingIndependentCodePoints> cicp;
    Optional<ByteBuffer> icc_profile;
    OwnPtr<ExifMetadata> exif_metadata;

    ErrorOr<size_t> read_frames(png_structp, png_infop);
    ErrorOr<void> apply_exif_orientation();

    ErrorOr<void> read_all_frames()
    {
        // NOTE: We need to setjmp() here because libpng uses longjmp() for error handling.
        if (auto error_value = setjmp(png_jmpbuf(png_ptr)); error_value) {
            return Error::from_errno(error_value);
        }

        png_read_update_info(png_ptr, info_ptr);

        frame_count = TRY(read_frames(png_ptr, info_ptr));

        if (exif_metadata)
            TRY(apply_exif_orientation());
        return {};
    }
};

namespace {

struct Chunk {
    StringView type;
    ReadonlyBytes payload;
    u32 stored_crc { 0 };
};

u32 png_chunk_crc(StringView type, ReadonlyBytes data)
{
    uLong c = ::crc32(0L, Z_NULL, 0);
    c = ::crc32(c, bit_cast<Bytef const*>(type.characters_without_null_termination()), type.length());
    c = ::crc32(c, data.data(), static_cast<uInt>(data.size()));
    return static_cast<u32>(c);
}

ErrorOr<void> append_be_u32(ByteBuffer& out, u32 value)
{
    BigEndian<u32> big = value;
    return out.try_append(ReadonlyBytes { &big, sizeof big });
}

Optional<Chunk> read_chunk(ReadonlyBytes input, size_t& offset)
{
    if (offset > input.size() || input.size() - offset < 12)
        return {};
    u32 length = *bit_cast<BigEndian<u32> const*>(input.data() + offset);
    if (length > input.size() - offset - 12)
        return {};
    StringView type { bit_cast<char const*>(input.data() + offset + 4), 4 };
    auto payload = input.slice(offset + 8, length);
    u32 stored_crc = *bit_cast<BigEndian<u32> const*>(input.data() + offset + 8 + length);
    offset += 12 + length;
    return Chunk { type, payload, stored_crc };
}

// A maximal run of consecutive IDAT or fdAT chunks. Each PNG has at most one IDAT run (frame 0, if the image has one),
// plus zero or more fdAT runs (one per subsequent APNG frame, separated by fcTL chunks). Each run's payloads form an
// independent deflate stream that needs its own rewrap if it's raw deflate. `adler` carries the Adler-32 of the
// inflated output iff rewrapping succeeded; otherwise the run passes through verbatim.
struct DeflateRun {
    size_t start_chunk_index { 0 };
    size_t end_chunk_index { 0 };
    size_t prefix_skip { 0 }; // 0 for IDAT; 4 for fdAT (sequence_number before the deflate data)
    Optional<u32> adler;
};

// Bound inflated output at 256 MiB so a raw-deflate bomb can't expand unbounded before libpng ever sees the bytes.
// Input size is deliberately not capped: legitimate raw-deflate PNGs of any size should work, and the output cap
// already bounds CPU work per inflate attempt.
constexpr u64 max_inflated_bytes = 256 * 1024 * 1024;

// Try to prove one deflate run is a raw-deflate stream and compute its Adler-32. Returns the Adler-32 on success,
// OptionalNone if the run isn't raw deflate (empty, CRC-corrupt, or fails inflate). Errors bubble up only on
// allocation failure.
ErrorOr<Optional<u32>> try_rewrap_deflate_run(Vector<Chunk, 16> const& chunks, DeflateRun const& run)
{
    auto deflate_bytes_of = [&](Chunk const& chunk) -> ReadonlyBytes {
        if (chunk.payload.size() < run.prefix_skip)
            return {};
        return chunk.payload.slice(run.prefix_skip);
    };

    // Fast path: if we have at least 5 deflate bytes and byte 0's low nibble is 8, the byte is simultaneously a
    // plausible zlib CMF and the start of a raw-deflate BFINAL=0/BTYPE=00 stored block. The stored-block
    // interpretation requires bytes 1-4 to be LEN (LE u16) and NLEN (= ~LEN); if `LEN ^ NLEN != 0xFFFF` the stream
    // can't be raw deflate, so it must be zlib — skip. Anything else (fewer than 5 bytes, non-CMF nibble, or the
    // ~1/65536 random collision) falls through to the inflate attempt below, which is the definitive test.
    Array<u8, 5> prefix;
    size_t prefix_bytes_seen = 0;
    for (size_t i = run.start_chunk_index; i <= run.end_chunk_index && prefix_bytes_seen < prefix.size(); ++i) {
        auto data = deflate_bytes_of(chunks[i]);
        size_t take = min(data.size(), prefix.size() - prefix_bytes_seen);
        __builtin_memcpy(prefix.data() + prefix_bytes_seen, data.data(), take);
        prefix_bytes_seen += take;
    }
    if (prefix_bytes_seen == 0)
        return OptionalNone {};
    if (prefix_bytes_seen == prefix.size() && (prefix[0] & 0x0f) == 0x08) {
        u16 len = static_cast<u16>(prefix[1]) | (static_cast<u16>(prefix[2]) << 8);
        u16 nlen = static_cast<u16>(prefix[3]) | (static_cast<u16>(prefix[4]) << 8);
        if ((len ^ nlen) != 0xFFFF)
            return OptionalNone {};
    }

    // Verify stored CRCs on the first and last chunks of the run — those are the only ones we'll rewrite; middle
    // chunks pass through with their original CRCs, so libpng still validates them.
    auto const& first_chunk = chunks[run.start_chunk_index];
    auto const& last_chunk = chunks[run.end_chunk_index];
    if (png_chunk_crc(first_chunk.type, first_chunk.payload) != first_chunk.stored_crc)
        return OptionalNone {};
    if (run.end_chunk_index != run.start_chunk_index
        && png_chunk_crc(last_chunk.type, last_chunk.payload) != last_chunk.stored_crc) {
        return OptionalNone {};
    }

    // Stream-inflate deflate data across chunks without concatenating upfront; this avoids an O(input-size) allocation
    // for arbitrarily large raw-deflate PNGs. fdAT sequence_number prefixes are skipped per-chunk.
    z_stream stream {};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        return Error::from_string_literal("inflateInit2 failed");
    ScopeGuard end_inflate = [&] { inflateEnd(&stream); };

    uLong adler = ::adler32(0L, Z_NULL, 0);
    u64 total_inflated = 0;
    Array<u8, 64 * 1024> inflate_buf;
    bool saw_stream_end = false;
    for (size_t i = run.start_chunk_index; i <= run.end_chunk_index; ++i) {
        auto data = deflate_bytes_of(chunks[i]);
        if (saw_stream_end) {
            // Any more deflate bytes past Z_STREAM_END means this isn't a single-member raw deflate stream.
            if (!data.is_empty())
                return OptionalNone {};
            continue;
        }
        if (data.is_empty())
            continue;
        stream.next_in = const_cast<Bytef*>(data.data());
        stream.avail_in = static_cast<uInt>(data.size());
        while (stream.avail_in > 0) {
            stream.next_out = inflate_buf.data();
            stream.avail_out = static_cast<uInt>(inflate_buf.size());
            int rv = inflate(&stream, Z_NO_FLUSH);
            size_t produced = inflate_buf.size() - stream.avail_out;
            if (produced > 0) {
                adler = ::adler32(adler, inflate_buf.data(), static_cast<uInt>(produced));
                total_inflated += produced;
                if (total_inflated > max_inflated_bytes)
                    return OptionalNone {};
            }
            if (rv == Z_STREAM_END) {
                if (stream.avail_in != 0)
                    return OptionalNone {};
                saw_stream_end = true;
                break;
            }
            if (rv != Z_OK)
                return OptionalNone {};
            if (produced == 0 && stream.avail_in == 0)
                break; // need more input from the next chunk
        }
    }
    if (!saw_stream_end)
        return OptionalNone {};
    return static_cast<u32>(adler);
}

// If any of the PNG's deflate-carrying chunk runs (IDAT for frame 0, fdAT for subsequent APNG frames) is a raw-deflate
// member (no zlib header), rewrap those runs as valid zlib streams so libpng can decode them. The PNG spec mandates
// zlib-wrapped deflate, but raw-deflate IDAT/fdAT payloads appear in the wild and are commonly accepted by other
// decoders; libpng rejects them. Runs that are already valid zlib pass through unchanged.
//
// libpng's existing structural checks are preserved: non-consecutive IDATs are rejected, CRC corruption on any
// rewritten chunk is rejected, chunk-walk failures are rejected. OptionalNone leaves the bytes unchanged; ErrorOr
// errors only for allocation/IO failures.
ErrorOr<Optional<ByteBuffer>> maybe_rewrap_raw_deflate_idat(ReadonlyBytes input)
{
    constexpr Array<u8, 8> png_signature = { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };
    if (input.size() < png_signature.size())
        return OptionalNone {};
    for (size_t i = 0; i < png_signature.size(); ++i) {
        if (input[i] != png_signature[i])
            return OptionalNone {};
    }

    // Walk chunks into an inline-capacity Vector that keeps well-formed PNGs off the heap entirely.
    Vector<Chunk, 16> chunks;
    bool saw_any_idat = false;
    bool non_idat_seen_after_first_idat = false;
    {
        size_t offset = png_signature.size();
        while (true) {
            auto chunk = read_chunk(input, offset);
            if (!chunk.has_value())
                return OptionalNone {};
            auto type = chunk->type;
            TRY(chunks.try_append(chunk.release_value()));
            if (type == "IDAT"sv) {
                // PNG spec: all IDAT chunks must be consecutive.
                if (non_idat_seen_after_first_idat)
                    return OptionalNone {};
                saw_any_idat = true;
            } else if (saw_any_idat) {
                non_idat_seen_after_first_idat = true;
            }
            if (type == "IEND"sv)
                break;
        }
    }

    // Identify runs of consecutive IDAT or consecutive fdAT chunks.
    Vector<DeflateRun, 8> runs;
    for (size_t i = 0; i < chunks.size(); ++i) {
        auto const& chunk = chunks[i];
        size_t prefix_skip;
        if (chunk.type == "IDAT"sv)
            prefix_skip = 0;
        else if (chunk.type == "fdAT"sv)
            prefix_skip = 4;
        else
            continue;
        if (!runs.is_empty() && runs.last().prefix_skip == prefix_skip && runs.last().end_chunk_index == i - 1) {
            runs.last().end_chunk_index = i;
        } else {
            DeflateRun run;
            run.start_chunk_index = i;
            run.end_chunk_index = i;
            run.prefix_skip = prefix_skip;
            TRY(runs.try_append(move(run)));
        }
    }
    if (runs.is_empty())
        return OptionalNone {};

    // Try rewrapping each run independently.
    bool any_rewrap = false;
    for (auto& run : runs) {
        run.adler = TRY(try_rewrap_deflate_run(chunks, run));
        if (run.adler.has_value())
            any_rewrap = true;
    }
    if (!any_rewrap)
        return OptionalNone {};

    // Emit the new PNG. Non-run chunks pass through verbatim; non-rewrapped runs pass through verbatim; rewrapped runs
    // get `0x78 0x01` prepended to the first chunk's deflate data and the 4-byte Adler-32 appended to the last, with
    // fresh CRCs on just those two chunks.
    constexpr Array<u8, 2> zlib_header = { 0x78, 0x01 };
    ByteBuffer output;
    TRY(output.try_ensure_capacity(input.size() + 6 * runs.size()));
    TRY(output.try_append(ReadonlyBytes { png_signature.data(), png_signature.size() }));

    size_t run_cursor = 0;
    for (size_t i = 0; i < chunks.size(); ++i) {
        auto const& chunk = chunks[i];
        while (run_cursor < runs.size() && runs[run_cursor].end_chunk_index < i)
            ++run_cursor;
        DeflateRun const* active_run = nullptr;
        if (run_cursor < runs.size()
            && runs[run_cursor].start_chunk_index <= i && i <= runs[run_cursor].end_chunk_index
            && runs[run_cursor].adler.has_value()) {
            active_run = &runs[run_cursor];
        }

        bool is_first = active_run && i == active_run->start_chunk_index;
        bool is_last = active_run && i == active_run->end_chunk_index;
        if (!is_first && !is_last) {
            TRY(append_be_u32(output, static_cast<u32>(chunk.payload.size())));
            TRY(output.try_append(chunk.type.bytes()));
            TRY(output.try_append(chunk.payload));
            TRY(append_be_u32(output, chunk.stored_crc));
            continue;
        }

        ByteBuffer new_payload;
        TRY(new_payload.try_ensure_capacity(chunk.payload.size() + 6));
        TRY(new_payload.try_append(chunk.payload.slice(0, active_run->prefix_skip)));
        if (is_first)
            TRY(new_payload.try_append(ReadonlyBytes { zlib_header.data(), zlib_header.size() }));
        TRY(new_payload.try_append(chunk.payload.slice(active_run->prefix_skip)));
        if (is_last) {
            BigEndian<u32> adler_be = *active_run->adler;
            TRY(new_payload.try_append(ReadonlyBytes { &adler_be, sizeof adler_be }));
        }
        TRY(append_be_u32(output, static_cast<u32>(new_payload.size())));
        TRY(output.try_append(chunk.type.bytes()));
        TRY(output.try_append(new_payload.bytes()));
        TRY(append_be_u32(output, png_chunk_crc(chunk.type, new_payload.bytes())));
    }

    return output;
}

}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> PNGImageDecoderPlugin::create(ReadonlyBytes bytes)
{
    auto decoder = adopt_own(*new PNGImageDecoderPlugin(bytes));
    TRY(decoder->initialize());

    auto result = decoder->m_context->read_all_frames();
    if (result.is_error()) {
        // NOTE: If we didn't fail in initialize(), that means we have size information.
        //       We can create a single-frame bitmap with that size and return it.
        //       This is weird, but kinda matches the behavior of other browsers.
        auto bitmap = TRY(Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Premultiplied, decoder->m_context->size));
        decoder->m_context->frame_descriptors.append({ move(bitmap), 0 });
        decoder->m_context->frame_count = 1;
        return decoder;
    }

    return decoder;
}

PNGImageDecoderPlugin::PNGImageDecoderPlugin(ReadonlyBytes data)
    : m_context(adopt_own(*new PNGLoadingContext))
{
    m_context->data = data;
}

size_t PNGImageDecoderPlugin::first_animated_frame_index()
{
    return 0;
}

IntSize PNGImageDecoderPlugin::size()
{
    return m_context->size;
}

bool PNGImageDecoderPlugin::is_animated()
{
    return m_context->frame_count > 1;
}

size_t PNGImageDecoderPlugin::loop_count()
{
    return m_context->loop_count;
}

size_t PNGImageDecoderPlugin::frame_count()
{
    return m_context->frame_count;
}

int PNGImageDecoderPlugin::frame_duration(size_t index)
{
    if (index >= m_context->frame_descriptors.size())
        return 0;
    return m_context->frame_descriptors[index].duration;
}

ErrorOr<ImageFrameDescriptor> PNGImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (index >= m_context->frame_descriptors.size())
        return Error::from_errno(EINVAL);

    return m_context->frame_descriptors[index];
}

ErrorOr<Optional<Media::CodingIndependentCodePoints>> PNGImageDecoderPlugin::cicp()
{
    return m_context->cicp;
}

ErrorOr<Optional<ReadonlyBytes>> PNGImageDecoderPlugin::icc_data()
{
    if (m_context->icc_profile.has_value())
        return Optional<ReadonlyBytes>(*m_context->icc_profile);
    return OptionalNone {};
}

static void log_png_error(png_structp png_ptr, char const* error_message)
{
    dbgln("libpng error: {}", error_message);
    png_longjmp(png_ptr, 1);
}

static void log_png_warning(png_structp, char const* warning_message)
{
    dbgln("libpng warning: {}", warning_message);
}

ErrorOr<void> PNGImageDecoderPlugin::initialize()
{
    m_context->png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!m_context->png_ptr)
        return Error::from_string_view("Failed to allocate read struct"sv);

    m_context->info_ptr = png_create_info_struct(m_context->png_ptr);
    if (!m_context->info_ptr) {
        return Error::from_string_view("Failed to allocate info struct"sv);
    }

    if (auto error_value = setjmp(png_jmpbuf(m_context->png_ptr)); error_value) {
        return Error::from_errno(error_value);
    }

    // libpng rejects raw-deflate IDAT; other browsers accept it. Rewrap before libpng sees the bytes. See
    // maybe_rewrap_raw_deflate_idat for details.
    if (auto rewrapped = TRY(maybe_rewrap_raw_deflate_idat(m_context->data)); rewrapped.has_value()) {
        m_context->rewrapped_data = rewrapped.release_value();
        m_context->data = m_context->rewrapped_data.bytes();
    }

    png_set_read_fn(m_context->png_ptr, &m_context->data, [](png_structp png_ptr, png_bytep data, png_size_t length) {
        auto* read_data = reinterpret_cast<ReadonlyBytes*>(png_get_io_ptr(png_ptr));
        if (read_data->size() < length) {
            png_error(png_ptr, "Read error");
            return;
        }
        memcpy(data, read_data->data(), length);
        *read_data = read_data->slice(length);
    });

    png_set_error_fn(m_context->png_ptr, nullptr, log_png_error, log_png_warning);

    png_read_info(m_context->png_ptr, m_context->info_ptr);

    u32 width = 0;
    u32 height = 0;
    int bit_depth = 0;
    int color_type = 0;
    int interlace_type = 0;
    png_get_IHDR(m_context->png_ptr, m_context->info_ptr, &width, &height, &bit_depth, &color_type, &interlace_type, nullptr, nullptr);
    m_context->size = { static_cast<int>(width), static_cast<int>(height) };

    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(m_context->png_ptr);

    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(m_context->png_ptr);

    if (png_get_valid(m_context->png_ptr, m_context->info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(m_context->png_ptr);

    if (bit_depth == 16)
        png_set_strip_16(m_context->png_ptr);

    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(m_context->png_ptr);

    if (interlace_type != PNG_INTERLACE_NONE)
        png_set_interlace_handling(m_context->png_ptr);

    png_set_filler(m_context->png_ptr, 0xFF, PNG_FILLER_AFTER);
    png_set_bgr(m_context->png_ptr);

    png_byte color_primaries { 0 };
    png_byte transfer_function { 0 };
    png_byte matrix_coefficients { 0 };
    png_byte video_full_range_flag { 0 };
    if (png_get_cICP(m_context->png_ptr, m_context->info_ptr, &color_primaries, &transfer_function, &matrix_coefficients, &video_full_range_flag)) {
        Media::ColorPrimaries cp { color_primaries };
        Media::TransferCharacteristics tc { transfer_function };
        Media::MatrixCoefficients mc { matrix_coefficients };
        Media::VideoFullRangeFlag rf { video_full_range_flag };
        m_context->cicp = Media::CodingIndependentCodePoints { cp, tc, mc, rf };
    } else {
        char* profile_name = nullptr;
        int compression_type = 0;
        u8* profile_data = nullptr;
        u32 profile_len = 0;
        if (png_get_iCCP(m_context->png_ptr, m_context->info_ptr, &profile_name, &compression_type, &profile_data, &profile_len))
            m_context->icc_profile = TRY(ByteBuffer::copy(profile_data, profile_len));
    }

    u8* exif_data = nullptr;
    u32 exif_length = 0;
    int const num_exif_chunks = png_get_eXIf_1(m_context->png_ptr, m_context->info_ptr, &exif_length, &exif_data);
    if (num_exif_chunks > 0)
        m_context->exif_metadata = TRY(TIFFImageDecoderPlugin::read_exif_metadata({ exif_data, exif_length }));

    return {};
}

ErrorOr<void> PNGLoadingContext::apply_exif_orientation()
{
    auto orientation = exif_metadata->orientation().value_or(TIFF::Orientation::Default);
    if (orientation == TIFF::Orientation::Default)
        return {};

    for (auto& img_frame_descriptor : frame_descriptors) {
        auto& img = img_frame_descriptor.image;
        auto oriented_bmp = TRY(ExifOrientedBitmap::create(orientation, img->size(), img->format()));

        for (int y = 0; y < img->size().height(); ++y) {
            for (int x = 0; x < img->size().width(); ++x) {
                auto pixel = img->get_pixel(x, y);
                oriented_bmp.set_pixel(x, y, pixel.value());
            }
        }

        img_frame_descriptor.image = oriented_bmp.bitmap();
    }

    size = ExifOrientedBitmap::oriented_size(size, orientation);

    return {};
}

ErrorOr<size_t> PNGLoadingContext::read_frames(png_structp png_ptr, png_infop info_ptr)
{
    Vector<u8*> row_pointers;
    auto decode_frame = [&](IntSize frame_size) -> ErrorOr<NonnullRefPtr<Bitmap>> {
        auto frame_bitmap = TRY(Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Unpremultiplied, frame_size));

        row_pointers.resize_and_keep_capacity(frame_size.height());
        for (auto i = 0; i < frame_size.height(); ++i)
            row_pointers[i] = frame_bitmap->scanline_u8(i);

        png_read_image(png_ptr, row_pointers.data());
        return frame_bitmap;
    };

    if (png_get_acTL(png_ptr, info_ptr, &frame_count, &loop_count)) {
        // acTL chunk present: This is an APNG.
        png_set_acTL(png_ptr, info_ptr, frame_count, loop_count);

        // Conceptually, at the beginning of each play the output buffer must be completely initialized to a fully transparent black rectangle, with width and height dimensions from the `IHDR` chunk.
        auto output_buffer = TRY(Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Unpremultiplied, size));
        auto painter = Painter::create(output_buffer);
        size_t animation_frame_count = 0;

        for (size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
            png_read_frame_head(png_ptr, info_ptr);
            u32 width = 0;
            u32 height = 0;
            u32 x = 0;
            u32 y = 0;
            u16 delay_num = 0;
            u16 delay_den = 0;
            u8 dispose_op = PNG_DISPOSE_OP_NONE;
            u8 blend_op = PNG_BLEND_OP_SOURCE;

            auto duration_ms = [&]() -> int {
                if (delay_num == 0)
                    return 1;
                u32 const denominator = delay_den != 0 ? static_cast<u32>(delay_den) : 100u;
                auto unsigned_duration_ms = (delay_num * 1000) / denominator;
                if (unsigned_duration_ms > INT_MAX)
                    return INT_MAX;
                return static_cast<int>(unsigned_duration_ms);
            };

            bool has_fcTL = png_get_valid(png_ptr, info_ptr, PNG_INFO_fcTL);
            if (has_fcTL) {
                png_get_next_frame_fcTL(png_ptr, info_ptr, &width, &height, &x, &y, &delay_num, &delay_den, &dispose_op, &blend_op);
            } else {
                width = png_get_image_width(png_ptr, info_ptr);
                height = png_get_image_height(png_ptr, info_ptr);
            }
            auto frame_rect = FloatRect { x, y, width, height };

            auto decoded_frame_bitmap = TRY(decode_frame({ width, height }));

            if (!has_fcTL)
                continue;

            RefPtr<Bitmap> prev_output_buffer;
            if (dispose_op == PNG_DISPOSE_OP_PREVIOUS) // Only actually clone if it's necessary
                prev_output_buffer = TRY(output_buffer->clone());

            switch (blend_op) {
            case PNG_BLEND_OP_SOURCE:
                // All color components of the frame, including alpha, overwrite the current contents of the frame's output buffer region.
                painter->draw_bitmap(frame_rect, Gfx::ImmutableBitmap::create(*decoded_frame_bitmap), decoded_frame_bitmap->rect(), Gfx::ScalingMode::NearestNeighbor, {}, 1.0f, Gfx::CompositingAndBlendingOperator::Copy);
                break;
            case PNG_BLEND_OP_OVER:
                // The frame should be composited onto the output buffer based on its alpha, using a simple OVER operation as described in the "Alpha Channel Processing" section of the PNG specification.
                painter->draw_bitmap(frame_rect, Gfx::ImmutableBitmap::create(*decoded_frame_bitmap), decoded_frame_bitmap->rect(), ScalingMode::NearestNeighbor, {}, 1.0f, Gfx::CompositingAndBlendingOperator::SourceOver);
                break;
            default:
                VERIFY_NOT_REACHED();
            }

            animation_frame_count++;
            frame_descriptors.append({ TRY(output_buffer->clone()), duration_ms() });

            switch (dispose_op) {
            case PNG_DISPOSE_OP_NONE:
                // No disposal is done on this frame before rendering the next; the contents of the output buffer are left as is.
                break;
            case PNG_DISPOSE_OP_BACKGROUND:
                // The frame's region of the output buffer is to be cleared to fully transparent black before rendering the next frame.
                painter->clear_rect(frame_rect, Gfx::Color::Transparent);
                break;
            case PNG_DISPOSE_OP_PREVIOUS:
                // The frame's region of the output buffer is to be reverted to the previous contents before rendering the next frame.
                painter->draw_bitmap(frame_rect, Gfx::ImmutableBitmap::create(*prev_output_buffer), IntRect { x, y, width, height }, Gfx::ScalingMode::NearestNeighbor, {}, 1.0f, Gfx::CompositingAndBlendingOperator::Copy);
                break;
            default:
                VERIFY_NOT_REACHED();
            }
        }

        frame_count = animation_frame_count;

        // If we didn't find any valid animation frames with fcTL chunks, fall back to using the base IDAT data as a single frame.
        if (frame_count == 0) {
            auto frame_bitmap = TRY(decode_frame(size));
            frame_descriptors.append({ move(frame_bitmap), 0 });
            frame_count = 1;
        }
    } else {
        // This is a single-frame PNG.
        frame_count = 1;
        loop_count = 0;

        auto decoded_frame_bitmap = TRY(decode_frame(size));
        frame_descriptors.append({ move(decoded_frame_bitmap), 0 });
    }
    return frame_count;
}

PNGImageDecoderPlugin::~PNGImageDecoderPlugin() = default;

bool PNGImageDecoderPlugin::sniff(ReadonlyBytes data)
{
    auto constexpr png_signature_size_in_bytes = 8;
    if (data.size() < png_signature_size_in_bytes)
        return false;
    return png_sig_cmp(data.data(), 0, png_signature_size_in_bytes) == 0;
}

Optional<Metadata const&> PNGImageDecoderPlugin::metadata()
{
    if (m_context->exif_metadata)
        return *m_context->exif_metadata;
    return OptionalNone {};
}

}
