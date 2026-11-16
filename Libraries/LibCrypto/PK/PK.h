/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Variant.h>

namespace Crypto::PK {

// The parameters of an AlgorithmIdentifier: absent, NULL, or a named curve.
using AlgorithmParameters = Variant<Empty, nullptr_t, Span<u32 const>>;

// https://www.rfc-editor.org/rfc/rfc5208#section-5
ErrorOr<ByteBuffer> wrap_in_private_key_info(ReadonlyBytes private_key, Span<u32 const> algorithm, AlgorithmParameters = {});

// https://www.rfc-editor.org/rfc/rfc5280#section-4.1
ErrorOr<ByteBuffer> wrap_in_subject_public_key_info(ReadonlyBytes public_key, Span<u32 const> algorithm, AlgorithmParameters = {});

}
