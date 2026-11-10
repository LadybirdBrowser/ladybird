/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Message.h>
#include <LibIPC/RawAttachment.h>
#include <LibIPC/ReceivedMessageBytes.h>

namespace IPC {

// A message as Rust is handed it, with where what Rust makes of it goes: `libipc_rust::cpp_connection::RawMessage`.
struct FfiRawMessage {
    ReadonlyBytes bytes_including_header;
    ReadonlySpan<RawAttachment> attachments;
    void* outcome;
    void (*respond)(void* outcome, ReadonlyBytes, ReadonlySpan<RawAttachment>);
    void (*did_misbehave)(void* outcome, ReadonlyBytes what_the_peer_did);
};
static_assert(sizeof(FfiRawMessage) == 7 * sizeof(void*), "RawMessage of libipc_rust is written to match this by hand");

class RawMessage final : public Message {
public:
    struct Header {
        u32 endpoint_magic { 0 };
        i32 message_id { 0 };
    };
    static Optional<Header> read_header(ReadonlyBytes);

    RawMessage(Header, StringView message_name, ReceivedMessageBytes, Vector<Attachment>);
    virtual ~RawMessage() override;

    virtual u32 endpoint_magic() const override { return m_header.endpoint_magic; }
    virtual int message_id() const override { return m_header.message_id; }
    virtual StringView message_name() const override { return m_message_name; }
    virtual ErrorOr<MessageBuffer> encode() const override;

    struct OutcomeInRust {
        // What the sender did that it must not do, for which its connection is closed.
        Optional<ByteString> misbehavior;
        // The response of a synchronous message.
        OwnPtr<MessageBuffer> response;
    };
    // Rust decodes the message and hands it to a stub of its endpoint, which `hand_over` reaches with what it is given.
    OutcomeInRust hand_to_rust(Function<void(FfiRawMessage)> const& hand_over);

private:
    Header m_header;
    StringView m_message_name;
    ReceivedMessageBytes m_bytes;
    Vector<Attachment> m_attachments;
};

}
