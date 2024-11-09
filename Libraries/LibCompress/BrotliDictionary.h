/*
 * Copyright (c) 2022, Michiel Visser <opensource@webmichiel.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>

namespace Compress {

class BrotliDictionary {
public:
    enum TransformationOperation {
        Identity,
        FermentFirst,
        FermentAll,
        OmitFirst,
        OmitLast,
    };
    struct Transformation {
        StringView prefix;
        TransformationOperation operation;
        u8 operation_data;
        StringView suffix;
    };

    static ErrorOr<ByteBuffer> lookup_word(size_t index, size_t length);
};

}
