/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Containers/Matroska/Reader.h>
#include <LibMedia/Sniff.h>

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
        u8 header[minimum_header_size];
        Bytes header_bytes { header, minimum_header_size };

        TRY(stream_cursor.read_into(header_bytes));

        // 4. Let box-size be the four bytes from sequence[0] to sequence[3], interpreted as a 32-bit unsigned big-endian integer.
        u32 box_size = (static_cast<u32>(header[0]) << 24) | (static_cast<u32>(header[1]) << 16) | (static_cast<u32>(header[2]) << 8) | static_cast<u32>(header[3]);

        // 5. If length is less than box-size or if box-size modulo 4 is not equal to 0, return false.
        if ((box_size % 4) != 0)
            return false;

        // 6. If the four bytes from sequence[4] to sequence[7] are not equal to 0x66 0x74 0x79 0x70 ("ftyp"), return false.
        if (header[4] != 0x66 || header[5] != 0x74 || header[6] != 0x79 || header[7] != 0x70)
            return false;

        // 7. If the three bytes from sequence[8] to sequence[10] are equal to 0x6D 0x70 0x34 ("mp4"), return true.
        if (header[8] == 0x6D && header[9] == 0x70 && header[10] == 0x34)
            return true;

        u8 minor_and_reserved[4];
        Bytes minor_bytes { minor_and_reserved, 4 };
        TRY(stream_cursor.read_into(minor_bytes));

        // 8. Let bytes-read be 16.
        size_t bytes_read = 16;

        // 9. While bytes-read is less than box-size, continuously loop through these steps:
        while (bytes_read < box_size) {
            u8 brand[4];
            Bytes brand_bytes { brand, 4 };
            auto brand_read = TRY(stream_cursor.read_into(brand_bytes));

            // 1. If the three bytes from sequence[bytes-read] to sequence[bytes-read + 2] are equal to 0x6D 0x70 0x34 ("mp4"), return true.
            auto is_supported_brand = [](u8 b0, u8 b1, u8 b2) {
                bool is_mp4 = (b0 == 'm' && b1 == 'p' && b2 == '4');
                // NOTE: Spec doesn't mention that, but we also have to allow "qt" and "iso"
                bool is_qt = (b0 == 'q' && b1 == 't' && b2 == ' ');
                bool is_iso = (b0 == 'i' && b1 == 's' && b2 == 'o');
                return is_mp4 || is_qt || is_iso;
            };
            if (is_supported_brand(brand[0], brand[1], brand[2]))
                return true;

            // 2. Increment bytes-read by 4.
            bytes_read += brand_read;
        }

        // 10. Return false.
        return false;
    }();

    if (result.is_error())
        return false;
    return result.release_value();
}

}
