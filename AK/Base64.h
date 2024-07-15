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

ErrorOr<ByteBuffer> decode_base64(StringView);
ErrorOr<ByteBuffer> decode_base64url(StringView);

ErrorOr<String> encode_base64(ReadonlyBytes);
ErrorOr<String> encode_base64url(ReadonlyBytes);

}

#if USING_AK_GLOBALLY
using AK::decode_base64;
using AK::decode_base64url;
using AK::encode_base64;
using AK::encode_base64url;
#endif
