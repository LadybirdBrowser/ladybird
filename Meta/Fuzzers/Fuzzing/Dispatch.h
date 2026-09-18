/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/MemoryStream.h>
#include <LibIPC/Message.h>

namespace Fuzzing {

template<typename Message>
NonnullOwnPtr<Message> decode_wire(IPC::MessageBuffer& wire)
{
    Queue<IPC::Attachment> attachments;
    for (auto& attachment : wire.take_attachments())
        attachments.enqueue(move(attachment));
    FixedMemoryStream stream(wire.data().span());
    VERIFY(MUST(stream.read_value<u32>()) == Message::ENDPOINT_MAGIC);
    VERIFY(MUST(stream.read_value<i32>()) == Message::static_message_id());
    auto message = MUST(Message::decode(stream, attachments));
    VERIFY(attachments.is_empty() && stream.is_eof());
    return message;
}

// Exercise generated encoding, decoding, argument verification and real virtual
// receiver dispatch. Framing and socket scheduling are covered by separate targets.
template<typename Endpoint, typename Message>
auto dispatch(Endpoint& endpoint, Message message)
{
    auto wire = MUST(message.encode());
    auto decoded = decode_wire<Message>(wire);
    auto response = MUST(endpoint.handle(move(decoded)));
    if constexpr (requires { typename Message::ResponseType; }) {
        using Response = typename Message::ResponseType;
        VERIFY(response);
        auto decoded_response = decode_wire<Response>(*response);
        return move(*decoded_response);
    } else {
        VERIFY(!response);
    }
}

}
