/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Page/SpellingError.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::SpellingError const& error)
{
    TRY(encoder.encode(error.offset));
    TRY(encoder.encode(error.length));
    return {};
}

template<>
ErrorOr<Web::SpellingError> IPC::decode(Decoder& decoder)
{
    auto offset = TRY(decoder.decode<u32>());
    auto length = TRY(decoder.decode<u32>());
    return Web::SpellingError { offset, length };
}
