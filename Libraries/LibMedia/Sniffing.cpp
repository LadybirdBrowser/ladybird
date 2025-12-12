/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Containers/Matroska/Reader.h>
#include <LibMedia/Sniffing.h>

namespace Media {

// https://mimesniff.spec.whatwg.org/#matches-the-signature-for-webm
bool sniff_webm(IncrementallyPopulatedStream::Cursor& stream_cursor)
{
    return Matroska::Reader::sniff_webm(stream_cursor);
}

// https://mimesniff.spec.whatwg.org/#matches-the-signature-for-mp4
bool sniff_mp4(IncrementallyPopulatedStream::Cursor& stream_cursor)
{
    auto result = [&] -> DecoderErrorOr<bool> {
        // 1. Let sequence be the byte sequence to be matched, where sequence[s] is byte s in sequence and sequence[0] is the first byte in sequence.
        // 2. Let length be the number of bytes in sequence.
        // 3. If length is less than 12, return false.
        constexpr size_t minimum_header_size = 12;
        Array<u8, minimum_header_size> header;
        auto read_size = TRY(stream_cursor.read_into(header.span()));
        if (read_size < 12)
            return false;

        // 4. Let box-size be the four bytes from sequence[0] to sequence[3], interpreted as a 32-bit unsigned big-endian integer.
        u32 box_size = AK::convert_between_host_and_big_endian(header.span().trim(4).reinterpret<u32>()[0]);

        // 5. If length is less than box-size or if box-size modulo 4 is not equal to 0, return false.
        if ((box_size % 4) != 0)
            return false;

        // 6. If the four bytes from sequence[4] to sequence[7] are not equal to 0x66 0x74 0x79 0x70 ("ftyp"), return false.
        if (StringView(header.span().slice(4, 4)) != "ftyp"sv)
            return false;

        // 7. If the three bytes from sequence[8] to sequence[10] are equal to 0x6D 0x70 0x34 ("mp4"), return true.

        // NB: Though not specifically allowed by the spec, Chromium and Firefox both treat QuickTime as MP4, since
        //     the formats are compatible.
        //
        //     Also, some files don't specify an 'mp4' brand, but only 'isom' or 'iso' with a version number for the
        //     final character.
        // FIXME: When this is eventually used for mime sniffing, we should return any of the following mimetypes:
        //        - video/mp4: mp4, iso
        //        - video/quicktime: qt
        //        - image/avif: avif, avis
        auto is_supported_brand = [](ReadonlyBytes brand_bytes) {
            auto brand = StringView(brand_bytes);
            if (brand.starts_with("mp4"sv))
                return true;
            if (brand == "qt  "sv)
                return true;
            if (brand.starts_with("iso"sv))
                return true;
            return false;
        };

        if (is_supported_brand(header.span().slice(8, 4)))
            return true;

        Array<u8, 4> minor_version;
        if (TRY(stream_cursor.read_into(minor_version.span())) < minor_version.size())
            return false;

        // 8. Let bytes-read be 16.
        size_t bytes_read = 16;

        // 9. While bytes-read is less than box-size, continuously loop through these steps:
        while (bytes_read < box_size) {
            Array<u8, 4> minor_brand;
            if (TRY(stream_cursor.read_into(minor_brand.span())) < minor_brand.size())
                return false;

            // 1. If the three bytes from sequence[bytes-read] to sequence[bytes-read + 2] are equal to 0x6D 0x70 0x34 ("mp4"), return true.
            if (is_supported_brand(minor_brand.span()))
                return true;

            // 2. Increment bytes-read by 4.
            bytes_read += minor_brand.size();
        }

        // 10. Return false.
        return false;
    }();

    if (result.is_error())
        return false;
    return result.release_value();
}

}
