/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/String.h>
#include <AK/StringView.h>

namespace AK {

size_t size_required_to_decode_base64(StringView);

ErrorOr<ByteBuffer> decode_base64(StringView);
ErrorOr<ByteBuffer> decode_base64url(StringView);

enum class OmitPadding {
    No,
    Yes,
};

ErrorOr<String> encode_base64(ReadonlyBytes, OmitPadding = OmitPadding::No);
ErrorOr<String> encode_base64url(ReadonlyBytes, OmitPadding = OmitPadding::No);

}

#if USING_AK_GLOBALLY
using AK::decode_base64;
using AK::decode_base64url;
using AK::encode_base64;
using AK::encode_base64url;
#endif
