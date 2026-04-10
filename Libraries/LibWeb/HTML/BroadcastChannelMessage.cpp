/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/BroadcastChannelMessage.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::BroadcastChannelMessage const& message)
{
    TRY(encoder.encode(message.storage_key));
    TRY(encoder.encode(message.channel_name));
    TRY(encoder.encode(message.source_origin));
    TRY(encoder.encode(message.serialized_message));
    TRY(encoder.encode(message.source_process_id));
    TRY(encoder.encode(message.source_channel_id));
    return {};
}

template<>
ErrorOr<Web::HTML::BroadcastChannelMessage> decode(Decoder& decoder)
{
    auto storage_key = TRY(decoder.decode<Web::StorageAPI::StorageKey>());
    auto channel_name = TRY(decoder.decode<String>());
    auto source_origin = TRY(decoder.decode<URL::Origin>());
    auto serialized_message = TRY(decoder.decode<Web::HTML::SerializationRecord>());
    auto source_process_id = TRY(decoder.decode<i32>());
    auto source_channel_id = TRY(decoder.decode<u64>());

    return Web::HTML::BroadcastChannelMessage {
        .storage_key = move(storage_key),
        .channel_name = move(channel_name),
        .source_origin = move(source_origin),
        .serialized_message = move(serialized_message),
        .source_process_id = source_process_id,
        .source_channel_id = source_channel_id,
    };
}

}
