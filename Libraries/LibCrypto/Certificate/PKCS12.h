/*
 * Copyright (c) 2026, Kevin Bortis
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace Crypto::Certificate {

struct PKCS12Result {
    ByteBuffer certificate_pem;
    ByteBuffer private_key_pem;
    Vector<ByteBuffer> ca_chain_pem;
};

ErrorOr<PKCS12Result> parse_pkcs12(ReadonlyBytes pkcs12_data, StringView password);

}
