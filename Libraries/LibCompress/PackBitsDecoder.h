/*
 * Copyright (c) 2023, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>

namespace Compress::PackBits {

// This implements the PackBits compression scheme, aka run-length compression
// It is fairly simple and described here: https://web.archive.org/web/20080705155158/http://developer.apple.com/technotes/tn/tn1023.html
// But also in section:
//  - 7.4.5 RunLengthDecode Filter of the PDF specification
//  - Section 9: PackBits Compression of the TIFF specification

enum class CompatibilityMode {
    Original, // 128 is defined as no-op
    PDF,      // 128 is defined as end of stream
};

ErrorOr<ByteBuffer> decode_all(ReadonlyBytes bytes, Optional<u64> expected_output_size = {}, CompatibilityMode mode = CompatibilityMode::Original);

}
