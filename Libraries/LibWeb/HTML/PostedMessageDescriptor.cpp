/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/PostedMessageDescriptor.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::PostedMessageDescriptor const& message)
{
    TRY(encoder.encode(message.serialize_with_transfer_result));
    TRY(encoder.encode(message.target_origin));
    TRY(encoder.encode(message.source_origin));
    TRY(encoder.encode(message.source_navigable_id));
    return {};
}

template<>
ErrorOr<Web::HTML::PostedMessageDescriptor> decode(Decoder& decoder)
{
    using TargetOrigin = Variant<Utf16String, URL::Origin>;
    return Web::HTML::PostedMessageDescriptor {
        .serialize_with_transfer_result = TRY(decoder.decode<Web::HTML::SerializedTransferRecord>()),
        .target_origin = TRY(decoder.decode<TargetOrigin>()),
        .source_origin = TRY(decoder.decode<URL::Origin>()),
        .source_navigable_id = TRY(decoder.decode<Optional<Web::HTML::CrossProcessId>>()),
    };
}

}
