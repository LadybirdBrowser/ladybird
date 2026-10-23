/*
 * Copyright (c) 2023-2024, Kemal Zebari <kemalzebra@gmail.com>.
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::mime_type::MimeType;

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum SniffingContext {
    None,
    Browsing,
    Image,
    AudioOrVideo,
    Font,

    // Non-standard but used in cases where the spec expects us to only run
    // https://mimesniff.spec.whatwg.org/#sniffing-a-mislabeled-binary-resource
    TextOrBinary,
}

struct BytePatternTableRow {
    byte_pattern: &'static [u8],
    pattern_mask: &'static [u8],
    ignored_leading_bytes: &'static [u8],
    mime_type: &'static str,

    // NB: If the byte pattern has a tag-terminating byte, add a byte where this byte should be. The value itself is
    //     ignored in pattern_matching_algorithm() (see the NB in that algorithm for more details).
    is_tag_terminated: bool,
}

const fn row(
    byte_pattern: &'static [u8],
    pattern_mask: &'static [u8],
    ignored_leading_bytes: &'static [u8],
    mime_type: &'static str,
) -> BytePatternTableRow {
    BytePatternTableRow {
        byte_pattern,
        pattern_mask,
        ignored_leading_bytes,
        mime_type,
        is_tag_terminated: false,
    }
}

const fn tag_terminated_row(
    byte_pattern: &'static [u8],
    pattern_mask: &'static [u8],
    ignored_leading_bytes: &'static [u8],
    mime_type: &'static str,
) -> BytePatternTableRow {
    BytePatternTableRow {
        is_tag_terminated: true,
        ..row(byte_pattern, pattern_mask, ignored_leading_bytes, mime_type)
    }
}

const NO_IGNORED_BYTES: &[u8] = &[];

// https://mimesniff.spec.whatwg.org/#whitespace-byte
// A whitespace byte (abbreviated 0xWS) is any one of the following bytes: 0x09 (HT), 0x0A (LF), 0x0C (FF), 0x0D (CR), 0x20 (SP).
const WHITESPACE_BYTES: &[u8] = &[0x09, 0x0A, 0x0C, 0x0D, 0x20];

fn mime_type(essence: &str) -> MimeType {
    let (type_, subtype) = essence
        .split_once('/')
        .expect("the MIME types of the tables are essences");
    MimeType::new(type_.to_owned(), subtype.to_owned())
}

// https://mimesniff.spec.whatwg.org/#tag-terminating-byte
fn is_tag_terminating_byte(byte: u8) -> bool {
    // A tag-terminating byte (abbreviated 0xTT) is any one of the following bytes: 0x20 (SP), 0x3E (">").
    byte == 0x20 || byte == 0x3E
}

// https://mimesniff.spec.whatwg.org/#binary-data-byte
fn is_binary_data_byte(byte: u8) -> bool {
    //  A binary data byte is a byte in the range 0x00 to 0x08 (NUL to BS), the byte 0x0B (VT), a byte in
    //  the range 0x0E to 0x1A (SO to SUB), or a byte in the range 0x1C to 0x1F (FS to US).
    matches!(byte, 0x00..=0x08 | 0x0B | 0x0E..=0x1A | 0x1C..=0x1F)
}

// https://mimesniff.spec.whatwg.org/#pattern-matching-algorithm
fn pattern_matching_algorithm(input: &[u8], row: &BytePatternTableRow) -> bool {
    let pattern = row.byte_pattern;
    let mask = row.pattern_mask;
    let ignored = row.ignored_leading_bytes;

    // 1. Assert: pattern’s length is equal to mask’s length.
    assert_eq!(pattern.len(), mask.len());

    // 2. If input’s length is less than pattern’s length, return false.
    if input.len() < pattern.len() {
        return false;
    }

    // 3. Let s be 0.
    let mut s = 0;

    // 4. While s < input’s length:
    while s < input.len() {
        // 1. If ignored does not contain input[s], break.
        if !ignored.contains(&input[s]) {
            break;
        }

        // 2. Set s to s + 1.
        s += 1;
    }

    // 5. Let p be 0.
    let mut p = 0;

    // 6. While p < pattern’s length:
    while p < pattern.len() {
        // NB: Leading ignored bytes can leave fewer bytes of input than the pattern has.
        let Some(&byte) = input.get(s) else {
            return false;
        };

        // 1. Let maskedData be the result of applying the bitwise AND operator to input[s] and mask[p].
        let masked_data = byte & mask[p];

        // NB: This non-standard branch exists to avoid having to create 2 byte patterns just so that
        //     they can only differ by their tag-terminating byte (which could be a 0x20 or 0x3E byte).
        if row.is_tag_terminated && p + 1 == pattern.len() {
            return is_tag_terminating_byte(masked_data);
        }

        // 2. If maskedData is not equal to pattern[p], return false.
        if masked_data != pattern[p] {
            return false;
        }

        // 3. Set s to s + 1.
        s += 1;

        // 4. Set p to p + 1.
        p += 1;
    }

    // 7. Return true.
    true
}

fn match_a_pattern_table(input: &[u8], pattern_table: &[BytePatternTableRow]) -> Option<MimeType> {
    // 1. Execute the following steps for each row row in the following table:
    for row in pattern_table {
        // 1. Let patternMatched be the result of the pattern matching algorithm given input, the value in
        //    the first column of row, the value in the second column of row, and the value in the third
        //    column of row.
        let pattern_matched = pattern_matching_algorithm(input, row);

        // 2. If patternMatched is true, return the value in the fourth column of row.
        if pattern_matched {
            return Some(mime_type(row.mime_type));
        }
    }

    // 2. Return undefined.
    None
}

// https://mimesniff.spec.whatwg.org/#matching-an-image-type-pattern
fn match_an_image_type_pattern(input: &[u8]) -> Option<MimeType> {
    const PATTERN_TABLE: [BytePatternTableRow; 8] = [
        // A Windows Icon signature.
        row(
            b"\x00\x00\x01\x00",
            b"\xFF\xFF\xFF\xFF",
            NO_IGNORED_BYTES,
            "image/x-icon",
        ),
        // A Windows Cursor signature.
        row(
            b"\x00\x00\x02\x00",
            b"\xFF\xFF\xFF\xFF",
            NO_IGNORED_BYTES,
            "image/x-icon",
        ),
        // The string "BM", a BMP signature.
        row(b"\x42\x4D", b"\xFF\xFF", NO_IGNORED_BYTES, "image/bmp"),
        // The string "GIF87a", a GIF signature.
        row(
            b"\x47\x49\x46\x38\x37\x61",
            b"\xFF\xFF\xFF\xFF\xFF\xFF",
            NO_IGNORED_BYTES,
            "image/gif",
        ),
        // The string "GIF89a", a GIF signature.
        row(
            b"\x47\x49\x46\x38\x39\x61",
            b"\xFF\xFF\xFF\xFF\xFF\xFF",
            NO_IGNORED_BYTES,
            "image/gif",
        ),
        // The string "RIFF" followed by four bytes followed by the string "WEBPVP".
        row(
            b"\x52\x49\x46\x46\x00\x00\x00\x00\x57\x45\x42\x50\x56\x50",
            b"\xFF\xFF\xFF\xFF\x00\x00\x00\x00\xFF\xFF\xFF\xFF\xFF\xFF",
            NO_IGNORED_BYTES,
            "image/webp",
        ),
        // An error-checking byte followed by the string "PNG" followed by CR LF SUB LF, the PNG signature.
        row(
            b"\x89\x50\x4E\x47\x0D\x0A\x1A\x0A",
            b"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF",
            NO_IGNORED_BYTES,
            "image/png",
        ),
        // The JPEG Start of Image marker followed by the indicator byte of another marker.
        row(b"\xFF\xD8\xFF", b"\xFF\xFF\xFF", NO_IGNORED_BYTES, "image/jpeg"),
    ];
    match_a_pattern_table(input, &PATTERN_TABLE)
}

// https://mimesniff.spec.whatwg.org/#signature-for-mp4
fn matches_mp4_signature(sequence: &[u8]) -> bool {
    // 1. Let sequence be the byte sequence to be matched, where sequence[s] is byte s in sequence and sequence[0] is the first byte in sequence.

    // 2. Let length be the number of bytes in sequence.
    let length = sequence.len();

    // 3. If length is less than 12, return false.
    if length < 12 {
        return false;
    }

    // 4. Let box-size be the four bytes from sequence[0] to sequence[3], interpreted as a 32-bit unsigned big-endian integer.
    let box_size = u32::from_be_bytes([sequence[0], sequence[1], sequence[2], sequence[3]]) as usize;

    // 5. If length is less than box-size or if box-size modulo 4 is not equal to 0, return false.
    if length < box_size || !box_size.is_multiple_of(4) {
        return false;
    }

    // 6. If the four bytes from sequence[4] to sequence[7] are not equal to 0x66 0x74 0x79 0x70 ("ftyp"), return false.
    if &sequence[4..8] != b"\x66\x74\x79\x70" {
        return false;
    }

    // 7. If the three bytes from sequence[8] to sequence[10] are equal to 0x6D 0x70 0x34 ("mp4"), return true.
    if &sequence[8..11] == b"\x6D\x70\x34" {
        return true;
    }

    // 8. Let bytes-read be 16.
    let mut bytes_read = 16;

    // 9. While bytes-read is less than box-size, continuously loop through these steps:
    while bytes_read < box_size {
        // 1. If the three bytes from sequence[bytes-read] to sequence[bytes-read + 2] are equal to 0x6D 0x70 0x34 ("mp4"), return true.
        if sequence.get(bytes_read..bytes_read + 3) == Some(b"\x6D\x70\x34") {
            return true;
        }

        // 2. Increment bytes-read by 4.
        bytes_read += 4;
    }

    // 10. Return false.
    false
}

struct VintResult {
    #[allow(dead_code)]
    value: u64,
    length: usize,
}

// https://mimesniff.spec.whatwg.org/#parse-a-vint
fn parse_a_vint(sequence: &[u8], iter: usize) -> VintResult {
    // To parse a vint on a byte sequence sequence of size length, starting at index iter use the following steps:
    let sequence = &sequence[iter..];
    let length = sequence.len();

    // 1. let mask be 128.
    let mut mask: u8 = 128;

    // 2. Let max vint length be 8.
    let max_vint_length = 8;

    // 3. Let number size be 1.
    let mut number_size = 1;

    // 5. Let index be 0.
    // NB: The spec declares this after step four, but it is needed before to use in the loop.
    let mut index = 0;

    // 4. While number size is less than max vint length, and less than length, continuously loop through these steps:
    while number_size < max_vint_length && number_size < length {
        // 1. If the sequence[index] & mask is not zero, abort these steps.
        if sequence[index] & mask != 0 {
            break;
        }

        // 2. Let mask be the value of mask >> 1.
        mask >>= 1;

        // 3. Increment number size by one.
        number_size += 1;
    }

    // 6. Let parsed number be sequence[index] & ~mask.
    let mut parsed_number = u64::from(sequence[index] & !mask);

    // 7. Increment index by one.
    index += 1;

    // 8. Let bytes remaining be the value of number size - 1.
    let mut bytes_remaining = number_size - 1;

    // 9. While bytes remaining is not zero, execute there steps:
    while bytes_remaining != 0 {
        // 1. Let parsed number be parsed number << 8.
        parsed_number <<= 8;

        // 2. Let parsed number be parsed number | sequence[index].
        parsed_number |= u64::from(sequence[index]);

        // 3. Increment index by one.
        index += 1;

        // 4. If index is greater or equal than length, abort these steps.
        if index >= length {
            break;
        }

        // 5. Decrement bytes remaining by one.
        bytes_remaining -= 1;
    }

    // 10. Return parsed number and number size
    VintResult {
        value: parsed_number,
        length: number_size,
    }
}

// https://mimesniff.spec.whatwg.org/#signature-for-webm
fn matches_webm_signature(sequence: &[u8]) -> bool {
    // 1. Let sequence be the byte sequence to be matched, where sequence[s] is byte s in sequence and sequence[0] is the first byte in sequence.

    // 2. Let length be the number of bytes in sequence.
    let length = sequence.len();

    // 3. If length is less than 4, return false.
    if length < 4 {
        return false;
    }

    // 4. If the four bytes from sequence[0] to sequence[3], are not equal to 0x1A 0x45 0xDF 0xA3, return false.
    if !sequence.starts_with(b"\x1A\x45\xDF\xA3") {
        return false;
    }

    // 5. let iter be 4.
    let mut iter = 4;

    // 6. While iter is less than length and iter is less than 38, continuously loop through these steps:
    while iter < length && iter < 38 {
        // 1. If the two bytes from sequence[iter] to sequence[iter + 1] are equal to 0x42 0x82,
        if sequence.get(iter..iter + 2) == Some(b"\x42\x82") {
            // 1. increment iter by 2.
            iter += 2;

            // 2. If iter is greater or equal than length, abort these steps.
            if iter >= length {
                break;
            }

            // 3. Let number size be the result of parsing a vint starting at sequence[iter].
            let number_size = parse_a_vint(sequence, iter).length;

            // 4. Increment iter by number size.
            iter += number_size;

            // 5. If iter is greater than or equal to length - 4, abort these steps.
            if iter >= length - 4 {
                break;
            }

            // 6. Let matched be the result of matching a padded sequence 0x77 0x65 0x62 0x6D ("webm") on sequence at offset iter.
            let matched = &sequence[iter..iter + 4] == b"\x77\x65\x62\x6D";

            // 7. If matched is true, abort these steps and return true.
            if matched {
                return true;
            }
        }

        // 2. Increment iter by 1.
        iter += 1;
    }

    // 7. Return false.
    false
}

// https://mimesniff.spec.whatwg.org/#match-an-mp3-header
fn match_mp3_header(sequence: &[u8], s: usize) -> bool {
    // To match an mp3 header, using a byte sequence sequence of length length at offset s execute these steps:
    let length = sequence.len();

    // 1. If length is less than 4, return false.
    if length - s < 4 {
        return false;
    }

    // 2. If sequence[s] is not equal to 0xff and sequence[s + 1] & 0xe0 is not equal to 0xe0, return false.
    // NB: The spec is wrong here, this should be or.
    if sequence[s] != 0xff || sequence[s + 1] & 0xe0 != 0xe0 {
        return false;
    }

    // 3. Let layer be the result of sequence[s + 1] & 0x06 >> 1.
    let layer = (sequence[s + 1] & 0x06) >> 1;

    // 4. If layer is 0, return false.
    if layer == 0 {
        return false;
    }

    // 5. Let bit-rate be sequence[s + 2] & 0xf0 >> 4.
    let bit_rate = (sequence[s + 2] & 0xf0) >> 4;

    // 6. If bit-rate is 15, return false.
    if bit_rate == 15 {
        return false;
    }

    // 7. Let sample-rate be sequence[s + 2] & 0x0c >> 2.
    let sample_rate = (sequence[s + 2] & 0x0c) >> 2;

    // 8. If sample-rate is 3, return false.
    if sample_rate == 3 {
        return false;
    }

    // FIXME: 9. Let freq be the value given by sample-rate in the table sample-rate.

    // FIXME: 10. Let final-layer be the result of 4 - (sequence[s + 1]).

    // FIXME: 11. If final-layer & 0x06 >> 1 is not 3, return false.

    // 12. Return true.
    true
}

// https://mimesniff.spec.whatwg.org/#matches-the-signature-for-mp3-without-id3
// FIXME: Instead of implementing full spec, only matches single-frame mp3 header due to the issues stated at (https://github.com/whatwg/mimesniff/issues/70)
fn matches_mp3_no_id3_signature(sequence: &[u8]) -> bool {
    // 1. Let sequence be the byte sequence to be matched, where sequence[s] is byte s in sequence and sequence[0] is the first byte in sequence.

    // FIXME: 2. Let length be the number of bytes in sequence.

    // FIXME: 3. Initialize s to 0.

    // 4. If the result of the operation match mp3 header is false, return false.
    if !match_mp3_header(sequence, 0) {
        return false;
    }

    // FIXME: 5. Parse an mp3 frame on sequence at offset s

    // FIXME: 6. Let skipped-bytes the return value of the execution of mp3 framesize computation

    // FIXME: 7. If skipped-bytes is less than 4, or skipped-bytes is greater than s - length, return false.

    // FIXME: 8. Increment s by skipped-bytes.

    // FIXME: 9. If the result of the operation match mp3 header operation is false, return false, else, return true.
    true
}

// https://mimesniff.spec.whatwg.org/#matching-an-audio-or-video-type-pattern
fn match_an_audio_or_video_type_pattern(input: &[u8]) -> Option<MimeType> {
    // 1. Execute the following steps for each row row in the following table:
    const PATTERN_TABLE: [BytePatternTableRow; 6] = [
        // The string "FORM" followed by four bytes followed by the string "AIFF", the AIFF signature.
        row(
            b"\x46\x4F\x52\x4D\x00\x00\x00\x00\x41\x49\x46\x46",
            b"\xFF\xFF\xFF\xFF\x00\x00\x00\x00\xFF\xFF\xFF\xFF",
            NO_IGNORED_BYTES,
            "audio/aiff",
        ),
        //  The string "ID3", the ID3v2-tagged MP3 signature.
        row(b"\x49\x44\x33", b"\xFF\xFF\xFF", NO_IGNORED_BYTES, "audio/mpeg"),
        // The string "OggS" followed by NUL, the Ogg container signature.
        row(
            b"\x4F\x67\x67\x53\x00",
            b"\xFF\xFF\xFF\xFF\xFF",
            NO_IGNORED_BYTES,
            "application/ogg",
        ),
        // The string "MThd" followed by four bytes representing the number 6 in 32 bits (big-endian), the MIDI signature.
        row(
            b"\x4D\x54\x68\x64\x00\x00\x00\x06",
            b"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF",
            NO_IGNORED_BYTES,
            "audio/midi",
        ),
        // The string "RIFF" followed by four bytes followed by the string "AVI ", the AVI signature.
        row(
            b"\x52\x49\x46\x46\x00\x00\x00\x00\x41\x56\x49\x20",
            b"\xFF\xFF\xFF\xFF\x00\x00\x00\x00\xFF\xFF\xFF\xFF",
            NO_IGNORED_BYTES,
            "video/avi",
        ),
        // The string "RIFF" followed by four bytes followed by the string "WAVE", the WAVE signature.
        row(
            b"\x52\x49\x46\x46\x00\x00\x00\x00\x57\x41\x56\x45",
            b"\xFF\xFF\xFF\xFF\x00\x00\x00\x00\xFF\xFF\xFF\xFF",
            NO_IGNORED_BYTES,
            "audio/wave",
        ),
    ];
    if let Some(matched_type) = match_a_pattern_table(input, &PATTERN_TABLE) {
        return Some(matched_type);
    }

    // 2. If input matches the signature for MP4, return "video/mp4".
    if matches_mp4_signature(input) {
        return Some(mime_type("video/mp4"));
    }

    // 3. If input matches the signature for WebM, return "video/webm".
    if matches_webm_signature(input) {
        return Some(mime_type("video/webm"));
    }

    // 4. If input matches the signature for MP3 without ID3, return "audio/mpeg".
    if matches_mp3_no_id3_signature(input) {
        return Some(mime_type("audio/mpeg"));
    }

    // 5. Return undefined.
    None
}

// https://mimesniff.spec.whatwg.org/#matching-a-font-type-pattern
fn match_a_font_type_pattern(input: &[u8]) -> Option<MimeType> {
    const PATTERN_TABLE: [BytePatternTableRow; 6] = [
        // 34 bytes followed by the string "LP", the Embedded OpenType signature.
        row(
            b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x4C\x50",
            b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xFF\xFF",
            NO_IGNORED_BYTES,
            "application/vnd.ms-fontobject",
        ),
        // 4 bytes representing the version number 1.0, a TrueType signature.
        row(b"\x00\x01\x00\x00", b"\xFF\xFF\xFF\xFF", NO_IGNORED_BYTES, "font/ttf"),
        // The string "OTTO", the OpenType signature.
        row(b"\x4F\x54\x54\x4F", b"\xFF\xFF\xFF\xFF", NO_IGNORED_BYTES, "font/otf"),
        // The string "ttcf", the TrueType Collection signature.
        row(b"\x74\x74\x63\x66", b"\xFF\xFF\xFF\xFF", NO_IGNORED_BYTES, "font/collection"),
        // The string "wOFF", the Web Open Font Format 1.0 signature.
        row(b"\x77\x4F\x46\x46", b"\xFF\xFF\xFF\xFF", NO_IGNORED_BYTES, "font/woff"),
        // The string "wOF2", the Web Open Font Format 2.0 signature.
        row(b"\x77\x4F\x46\x32", b"\xFF\xFF\xFF\xFF", NO_IGNORED_BYTES, "font/woff2"),
    ];
    match_a_pattern_table(input, &PATTERN_TABLE)
}

// https://mimesniff.spec.whatwg.org/#matching-an-archive-type-pattern
fn match_an_archive_type_pattern(input: &[u8]) -> Option<MimeType> {
    const PATTERN_TABLE: [BytePatternTableRow; 3] = [
        // The GZIP archive signature.
        row(b"\x1F\x8B\x08", b"\xFF\xFF\xFF", NO_IGNORED_BYTES, "application/x-gzip"),
        // The string "PK" followed by ETX EOT, the ZIP archive signature.
        row(
            b"\x50\x4B\x03\x04",
            b"\xFF\xFF\xFF\xFF",
            NO_IGNORED_BYTES,
            "application/zip",
        ),
        // The string "Rar!" followed by SUB BEL NUL, the RAR 4.x archive signature.
        row(
            b"\x52\x61\x72\x21\x1A\x07\x00",
            b"\xFF\xFF\xFF\xFF\xFF\xFF\xFF",
            NO_IGNORED_BYTES,
            "application/x-rar-compressed",
        ),
    ];
    match_a_pattern_table(input, &PATTERN_TABLE)
}

// https://mimesniff.spec.whatwg.org/#resource
struct Resource<'a> {
    // https://mimesniff.spec.whatwg.org/#supplied-mime-type
    // A supplied MIME type, the MIME type determined by the supplied MIME type detection algorithm.
    supplied_mime_type: Option<MimeType>,

    // https://mimesniff.spec.whatwg.org/#check-for-apache-bug-flag
    // A check-for-apache-bug flag, which defaults to unset.
    check_for_apache_bug_flag: bool,

    // https://mimesniff.spec.whatwg.org/#no-sniff-flag
    // A no-sniff flag, which defaults to set if the user agent does not wish to perform sniffing on the resource and unset otherwise.
    no_sniff: bool,

    // https://mimesniff.spec.whatwg.org/#computed-mime-type
    // A computed MIME type, the MIME type determined by the MIME type sniffing algorithm.
    computed_mime_type: MimeType,

    // https://mimesniff.spec.whatwg.org/#resource-header
    // A resource header is the byte sequence at the beginning of a resource, as determined by reading the resource header.
    resource_header: &'a [u8],
}

/// The computed MIME type of the resource whose data is `data`.
pub fn sniff(
    data: &[u8],
    sniffing_context: SniffingContext,
    scheme: &str,
    supplied_type: Option<MimeType>,
    no_sniff: bool,
) -> MimeType {
    let mut resource = Resource {
        supplied_mime_type: None,
        check_for_apache_bug_flag: false,
        no_sniff,
        // NB: Non-standard but for cases where pattern matching fails, let's fall back to the safest MIME type.
        computed_mime_type: mime_type("application/octet-stream"),
        resource_header: read_the_resource_header(data),
    };
    resource.supplied_mime_type_detection_algorithm(scheme, supplied_type);
    resource.context_specific_sniffing_algorithm(sniffing_context);
    resource.computed_mime_type
}

// https://mimesniff.spec.whatwg.org/#read-the-resource-header
fn read_the_resource_header(data: &[u8]) -> &[u8] {
    // 1. Let buffer be a byte sequence.
    // 2. Read bytes of the resource into buffer until one of the following conditions is met:
    //      - the end of the resource is reached.
    //      - the number of bytes in buffer is greater than or equal to 1445.
    //      - a reasonable amount of time has elapsed, as determined by the user agent.
    // FIXME: The spec expects us to be reading from a stream. Reimplement this spec step once
    //        we have greater support for streaming in areas that calls on this API.
    const MAX_SNIFF_SIZE: usize = 1445;

    // 3. The resource header is buffer.
    &data[..data.len().min(MAX_SNIFF_SIZE)]
}

impl Resource<'_> {
    // https://mimesniff.spec.whatwg.org/#supplied-mime-type-detection-algorithm
    // NB: Parameters are non-standard.
    fn supplied_mime_type_detection_algorithm(&mut self, scheme: &str, supplied_type: Option<MimeType>) {
        // 1. Let supplied-type be null.
        // 2. If the resource is retrieved via HTTP, execute the following steps:
        //        1. If one or more Content-Type headers are associated with the resource, execute the following steps:
        //               1. Set supplied-type to the value of the last Content-Type header associated with the resource.
        //               2. Set the check-for-apache-bug flag if supplied-type is exactly equal to one of the values in the following table:
        // NB: Non-standard but this algorithm expects the caller to handle step 2.1.1.
        if let Some(supplied_type) = &supplied_type
            && matches!(scheme, "http" | "https")
        {
            // NB: The spec expects a space between the semicolon and the start of the charset parameter. However, we will
            //     lose this space because MimeType::parse() ignores any spaces found there.
            const APACHE_BUG_MIME_TYPES: [&str; 4] = [
                "text/plain",
                "text/plain;charset=ISO-8859-1",
                "text/plain;charset=iso-8859-1",
                "text/plain;charset=UTF-8",
            ];

            let serialized_supplied_type = supplied_type.serialize();
            self.check_for_apache_bug_flag = APACHE_BUG_MIME_TYPES.contains(&serialized_supplied_type.as_str());
        }

        // 3. If the resource is retrieved directly from the file system, set supplied-type
        //    to the MIME type provided by the file system.
        // 4. If the resource is retrieved via another protocol (such as FTP), set
        //    supplied-type to the MIME type as determined by that protocol, if any.
        // 5. If supplied-type is not a MIME type, the supplied MIME type is undefined.
        //    Abort these steps.
        // 6. The supplied MIME type is supplied-type.
        // NB: The expectation is for the caller to handle these spec steps.
        self.supplied_mime_type = supplied_type;
    }

    // https://mimesniff.spec.whatwg.org/#mime-type-sniffing-algorithm
    fn mime_type_sniffing_algorithm(&mut self) {
        // 1. If the supplied MIME type is an XML MIME type or HTML MIME type, the computed MIME type is the supplied MIME type.
        //    Abort these steps.
        if let Some(supplied_mime_type) = &self.supplied_mime_type
            && (supplied_mime_type.essence().is_xml() || supplied_mime_type.essence().is_html())
        {
            self.computed_mime_type = supplied_mime_type.clone();
            return;
        }

        // 2. If the supplied MIME type is undefined or if the supplied MIME type’s essence
        //    is "unknown/unknown", "application/unknown", or "*/*", execute the rules for
        //    identifying an unknown MIME type with the sniff-scriptable flag equal to the
        //    inverse of the no-sniff flag and abort these steps.
        let Some(supplied_mime_type) = self.supplied_mime_type.clone().filter(|supplied_mime_type| {
            let essence = supplied_mime_type.essence();
            !matches!(
                (essence.type_, essence.subtype),
                ("unknown" | "application", "unknown") | ("*", "*")
            )
        }) else {
            self.computed_mime_type = self.rules_for_identifying_an_unknown_mime_type(!self.no_sniff);
            return;
        };

        // 3. If the no-sniff flag is set, the computed MIME type is the supplied MIME type.
        //    Abort these steps.
        if self.no_sniff {
            self.computed_mime_type = supplied_mime_type;
            return;
        }

        // 4. If the check-for-apache-bug flag is set, execute the rules for distinguishing
        //    if a resource is text or binary and abort these steps.
        if self.check_for_apache_bug_flag {
            self.rules_for_distinguishing_if_a_resource_is_text_or_binary();
            return;
        }

        // FIXME: 5. If the supplied MIME type is an image MIME type supported by the user agent, let matched-type be
        //    the result of executing the image type pattern matching algorithm with the resource header as
        //    the byte sequence to be matched.

        // 6. If matched-type is not undefined, the computed MIME type is matched-type.
        //    Abort these steps.

        // FIXME: 7. If the supplied MIME type is an audio or video MIME type supported by the user agent, let matched-type be
        //    the result of executing the audio or video type pattern matching algorithm with the resource header as
        //    the byte sequence to be matched.

        // 8. If matched-type is not undefined, the computed MIME type is matched-type.
        //    Abort these steps.

        // 9. The computed MIME type is the supplied MIME type.
        self.computed_mime_type = supplied_mime_type;
    }

    // https://mimesniff.spec.whatwg.org/#rules-for-identifying-an-unknown-mime-type
    fn rules_for_identifying_an_unknown_mime_type(&self, sniff_scriptable: bool) -> MimeType {
        // 1. If the sniff-scriptable flag is set, execute the following steps for each row row in the following table:
        if sniff_scriptable {
            const PATTERN_TABLE: [BytePatternTableRow; 19] = [
                // The case-insensitive string "<!DOCTYPE HTML" followed by a tag-terminating byte.
                tag_terminated_row(
                    b"\x3C\x21\x44\x4F\x43\x54\x59\x50\x45\x20\x48\x54\x4D\x4C\x00",
                    b"\xFF\xFF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xFF\xDF\xDF\xDF\xDF\xFF",
                    WHITESPACE_BYTES,
                    "text/html",
                ),
                // The case-insensitive string "<HTML" followed by a tag-terminating byte.
                tag_terminated_row(
                    b"\x3C\x48\x54\x4D\x4C\x00",
                    b"\xFF\xDF\xDF\xDF\xDF\xFF",
                    WHITESPACE_BYTES,
                    "text/html",
                ),
                // The case-insensitive string "<HEAD" followed by a tag-terminating byte.
                tag_terminated_row(
                    b"\x3C\x48\x45\x41\x44\x00",
                    b"\xFF\xDF\xDF\xDF\xDF\xFF",
                    WHITESPACE_BYTES,
                    "text/html",
                ),
                // The case-insensitive string "<SCRIPT" followed by a tag-terminating byte.
                tag_terminated_row(
                    b"\x3C\x53\x43\x52\x49\x50\x54\x00",
                    b"\xFF\xDF\xDF\xDF\xDF\xDF\xDF\xFF",
                    WHITESPACE_BYTES,
                    "text/html",
                ),
                // The case-insensitive string "<IFRAME" followed by a tag-terminating byte.
                tag_terminated_row(
                    b"\x3C\x49\x46\x52\x41\x4D\x45\x00",
                    b"\xFF\xDF\xDF\xDF\xDF\xDF\xDF\xFF",
                    WHITESPACE_BYTES,
                    "text/html",
                ),
                // The case-insensitive string "<H1" followed by a tag-terminating byte.
                tag_terminated_row(b"\x3C\x48\x31\x00", b"\xFF\xDF\xFF\xFF", WHITESPACE_BYTES, "text/html"),
                // The case-insensitive string "<DIV" followed by a tag-terminating byte.
                tag_terminated_row(
                    b"\x3C\x44\x49\x56\x00",
                    b"\xFF\xDF\xDF\xDF\xFF",
                    WHITESPACE_BYTES,
                    "text/html",
                ),
                // The case-insensitive string "<FONT" followed by a tag-terminating byte.
                tag_terminated_row(
                    b"\x3C\x46\x4F\x4E\x54\x00",
                    b"\xFF\xDF\xDF\xDF\xDF\xFF",
                    WHITESPACE_BYTES,
                    "text/html",
                ),
                // The case-insensitive string "<TABLE" followed by a tag-terminating byte.
                tag_terminated_row(
                    b"\x3C\x54\x41\x42\x4C\x45\x00",
                    b"\xFF\xDF\xDF\xDF\xDF\xDF\xFF",
                    WHITESPACE_BYTES,
                    "text/html",
                ),
                // The case-insensitive string "<A" followed by a tag-terminating byte.
                tag_terminated_row(b"\x3C\x41\x00", b"\xFF\xDF\xFF", WHITESPACE_BYTES, "text/html"),
                // The case-insensitive string "<STYLE" followed by a tag-terminating byte.
                tag_terminated_row(
                    b"\x3C\x53\x54\x59\x4C\x45\x00",
                    b"\xFF\xDF\xDF\xDF\xDF\xDF\xFF",
                    WHITESPACE_BYTES,
                    "text/html",
                ),
                // The case-insensitive string "<TITLE" followed by a tag-terminating byte.
                tag_terminated_row(
                    b"\x3C\x54\x49\x54\x4C\x45\x00",
                    b"\xFF\xDF\xDF\xDF\xDF\xDF\xFF",
                    WHITESPACE_BYTES,
                    "text/html",
                ),
                // The case-insensitive string "<B" followed by a tag-terminating byte.
                tag_terminated_row(b"\x3C\x42\x00", b"\xFF\xDF\xFF", WHITESPACE_BYTES, "text/html"),
                // The case-insensitive string "<BODY" followed by a tag-terminating byte.
                tag_terminated_row(
                    b"\x3C\x42\x4F\x44\x59\x00",
                    b"\xFF\xDF\xDF\xDF\xDF\xFF",
                    WHITESPACE_BYTES,
                    "text/html",
                ),
                // The case-insensitive string "<BR" followed by a tag-terminating byte.
                tag_terminated_row(b"\x3C\x42\x52\x00", b"\xFF\xDF\xDF\xFF", WHITESPACE_BYTES, "text/html"),
                // The case-insensitive string "<P" followed by a tag-terminating byte.
                tag_terminated_row(b"\x3C\x50\x00", b"\xFF\xDF\xFF", WHITESPACE_BYTES, "text/html"),
                // The string "<!--" followed by a tag-terminating byte.
                tag_terminated_row(
                    b"\x3C\x21\x2D\x2D\x00",
                    b"\xFF\xFF\xFF\xFF\xFF",
                    WHITESPACE_BYTES,
                    "text/html",
                ),
                // The string "<?xml".
                row(
                    b"\x3C\x3F\x78\x6D\x6C",
                    b"\xFF\xFF\xFF\xFF\xFF",
                    WHITESPACE_BYTES,
                    "text/xml",
                ),
                // The string "%PDF-", the PDF signature.
                row(
                    b"\x25\x50\x44\x46\x2D",
                    b"\xFF\xFF\xFF\xFF\xFF",
                    NO_IGNORED_BYTES,
                    "application/pdf",
                ),
            ];

            // 1. Let patternMatched be the result of the pattern matching algorithm given resource’s resource header,
            //    the value in the first column of row, the value in the second column of row, and the value in the
            //    third column of row.
            // 2. If patternMatched is true, return the value in the fourth column of row.
            if let Some(matched_type) = match_a_pattern_table(self.resource_header, &PATTERN_TABLE) {
                return matched_type;
            }
        }

        // 2. Execute the following steps for each row row in the following table:
        const PATTERN_TABLE: [BytePatternTableRow; 4] = [
            // The string "%!PS-Adobe-", the PostScript signature.
            row(
                b"\x25\x21\x50\x53\x2D\x41\x64\x6F\x62\x65\x2D",
                b"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF",
                NO_IGNORED_BYTES,
                "application/postscript",
            ),
            // UTF-16BE BOM
            row(b"\xFE\xFF\x00\x00", b"\xFF\xFF\x00\x00", NO_IGNORED_BYTES, "text/plain"),
            // UTF-16LE BOM
            row(b"\xFF\xFE\x00\x00", b"\xFF\xFF\x00\x00", NO_IGNORED_BYTES, "text/plain"),
            // UTF-8 BOM
            row(b"\xEF\xBB\xBF\x00", b"\xFF\xFF\xFF\x00", NO_IGNORED_BYTES, "text/plain"),
        ];

        // 1. Let patternMatched be the result of the pattern matching algorithm given resource’s resource header,
        //    the value in the first column of row, the value in the second column of row, and the value in the
        //    third column of row.
        // 2. If patternMatched is true, return the value in the fourth column of row.
        if let Some(matched_type) = match_a_pattern_table(self.resource_header, &PATTERN_TABLE) {
            return matched_type;
        }

        // 3. Let matchedType be the result of executing the image type pattern matching algorithm given resource’s resource header.
        // 4. If matchedType is not undefined, return matchedType.
        if let Some(matched_type) = match_an_image_type_pattern(self.resource_header) {
            return matched_type;
        }

        // 5. Set matchedType to the result of executing the audio or video type pattern matching algorithm given resource’s resource header.
        // 6. If matchedType is not undefined, return matchedType.
        if let Some(matched_type) = match_an_audio_or_video_type_pattern(self.resource_header) {
            return matched_type;
        }

        // 7. Set matchedType to the result of executing the archive type pattern matching algorithm given resource’s resource header.
        // 8. If matchedType is not undefined, return matchedType.
        if let Some(matched_type) = match_an_archive_type_pattern(self.resource_header) {
            return matched_type;
        }

        // 9. If resource’s resource header contains no binary data bytes, return "text/plain".
        if !self.resource_header.iter().copied().any(is_binary_data_byte) {
            return mime_type("text/plain");
        }

        // 10. Return "application/octet-stream".
        mime_type("application/octet-stream")
    }

    // https://mimesniff.spec.whatwg.org/#sniffing-a-mislabeled-binary-resource
    fn rules_for_distinguishing_if_a_resource_is_text_or_binary(&mut self) {
        // 1. Let length be the number of bytes in the resource header.
        let length = self.resource_header.len();

        // 2. If length is greater than or equal to 2 and the first 2 bytes of the
        //    resource header are equal to 0xFE 0xFF (UTF-16BE BOM) or 0xFF 0xFE (UTF-16LE BOM), the computed MIME type is "text/plain".
        //    Abort these steps.
        if length >= 2
            && (self.resource_header.starts_with(b"\xFE\xFF") || self.resource_header.starts_with(b"\xFF\xFE"))
        {
            self.computed_mime_type = mime_type("text/plain");
            return;
        }

        // 3. If length is greater than or equal to 3 and the first 3 bytes of the resource header are equal to 0xEF 0xBB 0xBF (UTF-8 BOM),
        //    the computed MIME type is "text/plain".
        //    Abort these steps.
        if length >= 3 && self.resource_header.starts_with(b"\xEF\xBB\xBF") {
            self.computed_mime_type = mime_type("text/plain");
            return;
        }

        // 4. If the resource header contains no binary data bytes, the computed MIME type is "text/plain".
        //    Abort these steps.
        if !self.resource_header.iter().copied().any(is_binary_data_byte) {
            self.computed_mime_type = mime_type("text/plain");
        }

        // 5. The computed MIME type is "application/octet-stream".
        // NB: This is the default MIME type of the computed MIME type.
    }

    // https://mimesniff.spec.whatwg.org/#context-specific-sniffing-algorithm
    fn context_specific_sniffing_algorithm(&mut self, sniffing_context: SniffingContext) {
        // A context-specific sniffing algorithm determines the computed MIME type of a resource only if
        // the resource is a MIME type relevant to a particular context.
        if matches!(sniffing_context, SniffingContext::None | SniffingContext::Browsing) {
            // https://mimesniff.spec.whatwg.org/#sniffing-in-a-browsing-context
            // Use the MIME type sniffing algorithm.
            self.mime_type_sniffing_algorithm();
            return;
        }

        // NB: Non-standard but if the client expects us to not sniff, we shouldn't be doing any
        //     context-specific sniffing if we don't have to.
        if self.no_sniff
            && let Some(supplied_mime_type) = &self.supplied_mime_type
        {
            self.computed_mime_type = supplied_mime_type.clone();
            return;
        }

        match sniffing_context {
            SniffingContext::Image => self.rules_for_sniffing_images_specifically(),
            SniffingContext::AudioOrVideo => self.rules_for_sniffing_audio_or_video_specifically(),
            SniffingContext::Font => self.rules_for_sniffing_fonts_specifically(),
            SniffingContext::TextOrBinary => self.rules_for_distinguishing_if_a_resource_is_text_or_binary(),
            SniffingContext::None | SniffingContext::Browsing => unreachable!(),
        }
    }

    // https://mimesniff.spec.whatwg.org/#sniffing-in-an-image-context
    fn rules_for_sniffing_images_specifically(&mut self) {
        // 1. If the supplied MIME type is an XML MIME type, the computed MIME type is the supplied MIME type.
        //    Abort these steps.
        // NB: Non-standard but due to the mime type detection algorithm we need this sanity check.
        if let Some(supplied_mime_type) = &self.supplied_mime_type
            && supplied_mime_type.essence().is_xml()
        {
            self.computed_mime_type = supplied_mime_type.clone();
            return;
        }

        // 2. Let image-type-matched be the result of executing the image type pattern matching algorithm with
        //    the resource header as the byte sequence to be matched.
        // 3. If image-type-matched is not undefined, the computed MIME type is image-type-matched.
        //    Abort these steps.
        if let Some(matched_type) = match_an_image_type_pattern(self.resource_header) {
            self.computed_mime_type = matched_type;
            return;
        }

        // 4. The computed MIME type is the supplied MIME type.
        // NB: Non-standard but due to the mime type detection algorithm we need this sanity check.
        //     If the supplied mime type is undefined, we use computed mime type's default value.
        if let Some(supplied_mime_type) = &self.supplied_mime_type {
            self.computed_mime_type = supplied_mime_type.clone();
        }
    }

    // https://mimesniff.spec.whatwg.org/#sniffing-in-an-audio-or-video-context
    fn rules_for_sniffing_audio_or_video_specifically(&mut self) {
        // 1. If the supplied MIME type is an XML MIME type, the computed MIME type is the supplied MIME type.
        //    Abort these steps.
        // NB: Non-standard but due to the mime type detection algorithm we need this sanity check.
        if let Some(supplied_mime_type) = &self.supplied_mime_type
            && supplied_mime_type.essence().is_xml()
        {
            self.computed_mime_type = supplied_mime_type.clone();
            return;
        }

        // 2. Let audio-or-video-type-matched be the result of executing the audio or video type pattern matching
        //    algorithm with the resource header as the byte sequence to be matched.
        // 3. If audio-or-video-type-matched is not undefined, the computed MIME type is audio-or-video-type-matched.
        //    Abort these steps.
        if let Some(matched_type) = match_an_audio_or_video_type_pattern(self.resource_header) {
            self.computed_mime_type = matched_type;
            return;
        }

        // 4. The computed MIME type is the supplied MIME type.
        // NB: Non-standard but due to the mime type detection algorithm we need this sanity check.
        //     If the supplied mime type is undefined, we use computed mime type's default value.
        if let Some(supplied_mime_type) = &self.supplied_mime_type {
            self.computed_mime_type = supplied_mime_type.clone();
        }
    }

    // https://mimesniff.spec.whatwg.org/#sniffing-in-a-font-context
    fn rules_for_sniffing_fonts_specifically(&mut self) {
        // 1. If the supplied MIME type is an XML MIME type, the computed MIME type is the supplied MIME type.
        //    Abort these steps.
        // NB: Non-standard but due to the mime type detection algorithm we need this sanity check.
        if let Some(supplied_mime_type) = &self.supplied_mime_type
            && supplied_mime_type.essence().is_xml()
        {
            self.computed_mime_type = supplied_mime_type.clone();
            return;
        }

        // 2. Let font-type-matched be the result of executing the font type pattern matching algorithm with the
        //    resource header as the byte sequence to be matched.
        // 3. If font-type-matched is not undefined, the computed MIME type is font-type-matched.
        //    Abort these steps.
        if let Some(matched_type) = match_a_font_type_pattern(self.resource_header) {
            self.computed_mime_type = matched_type;
            return;
        }

        // 4. The computed MIME type is the supplied MIME type.
        // NB: Non-standard but due to the mime type detection algorithm we need this sanity check.
        //     If the supplied mime type is undefined, we use computed mime type's default value.
        if let Some(supplied_mime_type) = &self.supplied_mime_type {
            self.computed_mime_type = supplied_mime_type.clone();
        }
    }
}
