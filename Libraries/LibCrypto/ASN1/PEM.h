/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Span.h>

namespace Crypto {

enum PEMType {
    Certificate,
    PrivateKey,
};

ByteBuffer decode_pem(ReadonlyBytes);
ErrorOr<Vector<ByteBuffer>> decode_pems(ReadonlyBytes);
ErrorOr<ByteBuffer> encode_pem(ReadonlyBytes, PEMType = PEMType::Certificate);

}
