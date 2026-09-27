/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Random.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>

namespace Web::HTML {

// A new unique opaque string.
EnvironmentId EnvironmentId::generate()
{
    return EnvironmentId { generate_random_uuid() };
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::EnvironmentId const& id)
{
    return encoder.encode(id.to_string());
}

template<>
ErrorOr<Web::HTML::EnvironmentId> decode(Decoder& decoder)
{
    return Web::HTML::EnvironmentId { TRY(decoder.decode<String>()) };
}

}
