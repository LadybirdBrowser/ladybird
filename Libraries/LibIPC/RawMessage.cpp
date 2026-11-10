/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <LibIPC/RawMessage.h>

namespace IPC {

Optional<RawMessage::Header> RawMessage::read_header(ReadonlyBytes bytes)
{
    if (bytes.size() < sizeof(u32) + sizeof(i32))
        return {};
    Header header;
    AK::ByteReader::load(bytes.data(), header.endpoint_magic);
    AK::ByteReader::load(bytes.data() + sizeof(u32), header.message_id);
    return header;
}

RawMessage::RawMessage(Header header, StringView message_name, ReceivedMessageBytes bytes, Vector<Attachment> attachments)
    : m_header(header)
    , m_message_name(message_name)
    , m_bytes(move(bytes))
    , m_attachments(move(attachments))
{
}

RawMessage::~RawMessage() = default;

ErrorOr<MessageBuffer> RawMessage::encode() const
{
    return Error::from_string_literal("A message that arrived undecoded cannot be encoded");
}

RawMessage::OutcomeInRust RawMessage::hand_to_rust(Function<void(FfiRawMessage)> const& hand_over)
{
    auto attachments = release_raw_attachments(move(m_attachments));
    OutcomeInRust outcome;
    hand_over({
        .bytes_including_header = m_bytes.bytes(),
        .attachments = attachments,
        .outcome = &outcome,
        .respond = [](void* outcome, ReadonlyBytes bytes, ReadonlySpan<RawAttachment> attachments) { static_cast<OutcomeInRust*>(outcome)->response = make<MessageBuffer>(adopt_encoded_message(bytes, attachments)); },
        .did_misbehave = [](void* outcome, ReadonlyBytes what_the_peer_did) { static_cast<OutcomeInRust*>(outcome)->misbehavior = ByteString { what_the_peer_did }; },
    });
    return outcome;
}

}
