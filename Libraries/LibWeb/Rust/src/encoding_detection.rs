/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use chardetng::EncodingDetector;
use chardetng::Iso2022JpDetection;
use chardetng::Utf8Detection;

/// Attempts to detect the character encoding of a byte stream using frequency analysis.
///
/// This implements step 8 of the WHATWG encoding sniffing algorithm:
/// https://html.spec.whatwg.org/multipage/parsing.html#determining-the-character-encoding
///
/// # Safety
/// - `input` and `input_len` must describe a valid byte slice (or `input` may be null if
///   `input_len` is 0)
/// - `tld` if non-null, must describe a valid byte slice of `tld_len` bytes containing the
///   rightmost DNS label of the resource's host, with no dots, no uppercase, and only ASCII
///   characters — these constraints are required by chardetng and must be validated by the caller
/// - `out_encoding_name` and `out_encoding_name_len` must be non-null writable pointers
///
/// Returns `true` if an encoding was detected (always, unless the input pointer is invalid).
/// When `true` is returned, `*out_encoding_name` is set to a pointer into a static ASCII
/// string naming the detected encoding (e.g. `"windows-1252"`, `"Shift_JIS"`), and
/// `*out_encoding_name_len` is set to its byte length. The pointer is valid for the lifetime
/// of the process. When `false` is returned (only on null-pointer error), the output pointers
/// are left unmodified.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_detect_encoding(
    input: *const u8,
    input_len: usize,
    tld: *const u8,
    tld_len: usize,
    allow_utf8: bool,
    out_encoding_name: *mut *const u8,
    out_encoding_name_len: *mut usize,
) -> bool {
    unsafe {
        crate::abort_on_panic(|| {
            let Some(input_slice) = crate::bytes_from_raw(input, input_len) else {
                return false;
            };

            let tld_slice = if tld.is_null() || tld_len == 0 {
                None
            } else {
                Some(std::slice::from_raw_parts(tld, tld_len))
            };

            // Web browsers must use `Iso2022JpDetection::Deny` and `Utf8Detection::Deny` to
            // prevent charset confusion attacks. See the chardetng documentation for details.
            // Japanese pages using ISO-2022-JP will have declared it in a <meta> tag, which
            // is detected in step 5 (prescan) before this step is reached.
            // We always call `guess()` even for pure-ASCII input because chardetng still
            // tracks ESC sequences (ISO-2022-JP uses 7-bit escapes) and returns the
            // correct locale-based fallback (windows-1252 for generic TLD) with
            // `Utf8Detection::Deny` when no distinctive non-ASCII encoding evidence is found.
            let mut detector = EncodingDetector::new(Iso2022JpDetection::Deny);
            // Pass last=false because the caller may only be providing a sniff-bytes prefix
            // of a longer stream. chardetng docs: "If you want to perform detection on just
            // the prefix of a longer stream, do not pass last=true."
            detector.feed(input_slice, false);

            let utf8_detection = if allow_utf8 {
                Utf8Detection::Allow
            } else {
                Utf8Detection::Deny
            };
            let encoding = detector.guess(tld_slice, utf8_detection);
            let name = encoding.name().as_bytes();
            *out_encoding_name = name.as_ptr();
            *out_encoding_name_len = name.len();
            true
        })
    }
}
