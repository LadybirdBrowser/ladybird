/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>

namespace AK {

size_t size_required_to_decode_base64(StringView);
size_t size_required_to_decode_base64(Utf16View);

enum class LastChunkHandling {
    Loose,
    Strict,
    StopBeforePartial,
};

enum class Base64DecodeError {
    ExtraBits,
    InputRemainder,
    InvalidCharacter,
    InvalidData,
};

ErrorOr<ByteBuffer> decode_base64(StringView, LastChunkHandling = LastChunkHandling::Loose);
ErrorOr<ByteBuffer> decode_base64url(StringView, LastChunkHandling = LastChunkHandling::Loose);

struct InvalidBase64 {
    Base64DecodeError decode_error { Base64DecodeError::InvalidData };
    Error error;
    size_t valid_input_bytes { 0 };
};

// On success, these return the number of input bytes that were decoded. This might be less than the
// string length if the output buffer was not large enough.
ErrorOr<size_t, InvalidBase64> decode_base64_into(StringView, ByteBuffer&, LastChunkHandling = LastChunkHandling::Loose);
ErrorOr<size_t, InvalidBase64> decode_base64url_into(StringView, ByteBuffer&, LastChunkHandling = LastChunkHandling::Loose);
ErrorOr<size_t, InvalidBase64> decode_base64_into(Utf16View, ByteBuffer&, LastChunkHandling = LastChunkHandling::Loose);
ErrorOr<size_t, InvalidBase64> decode_base64url_into(Utf16View, ByteBuffer&, LastChunkHandling = LastChunkHandling::Loose);

enum class OmitPadding {
    No,
    Yes,
};

ErrorOr<String> encode_base64(ReadonlyBytes, OmitPadding = OmitPadding::No);
ErrorOr<String> encode_base64url(ReadonlyBytes, OmitPadding = OmitPadding::No);
ErrorOr<Utf16String> encode_base64_to_utf16(ReadonlyBytes, OmitPadding = OmitPadding::No);
ErrorOr<Utf16String> encode_base64url_to_utf16(ReadonlyBytes, OmitPadding = OmitPadding::No);

}

#if USING_AK_GLOBALLY
using AK::decode_base64;
using AK::decode_base64url;
using AK::encode_base64;
using AK::encode_base64_to_utf16;
using AK::encode_base64url;
using AK::encode_base64url_to_utf16;
#endif
