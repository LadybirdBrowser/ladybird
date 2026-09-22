/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fuzzing/BoundedInput.h"
#include <AK/MemoryStream.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/TransportSocket.h>
#include <fcntl.h>

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    if (size > 16384)
        return 0;
    Fuzzing::BoundedInput input({ data, size });
    auto queue = adopt_ref(*new IPC::SendQueue);
    struct ExpectedMessage {
        IPC::SocketMessageHeader header;
        Vector<u8> payload;
        size_t end_of_fds;
    };
    Vector<ExpectedMessage> messages;
    Vector<int> expected_fds;
    Vector<u8> sent_bytes;
    size_t sent_fd_count = 0;
    auto drain = [&](size_t maximum, u8 partial) {
        auto batch = queue->peek(maximum);
        auto repeated = queue->peek(maximum);
        VERIFY(batch.bytes == repeated.bytes && batch.fds == repeated.fds);
        VERIFY(batch.bytes.size() <= maximum);
        VERIFY(batch.fds.size() <= Core::LocalSocket::MAX_TRANSFER_FDS);
        VERIFY(batch.fds.size() <= expected_fds.size() - sent_fd_count);
        for (size_t i = 0; i < batch.fds.size(); ++i)
            VERIFY(batch.fds[i] == expected_fds[sent_fd_count + i]);
        if (batch.bytes.is_empty()) {
            VERIFY(batch.fds.is_empty());
            return false;
        }
        auto count = partial == 0 ? batch.bytes.size() : 1 + partial % batch.bytes.size();
        sent_bytes.append(batch.bytes.data(), count);
        sent_fd_count += batch.fds.size();
        queue->discard(count, batch.fds.size());
        // No byte of a message may be sent before that message's attachments.
        size_t offset = 0;
        for (auto const& message : messages) {
            if (sent_bytes.size() > offset)
                VERIFY(sent_fd_count >= message.end_of_fds);
            offset += sizeof(IPC::SocketMessageHeader) + message.payload.size();
        }
        return true;
    };
    for (size_t step = 0; step < 128 && !input.remaining().is_empty(); ++step) {
        auto operation = input.byte();
        if ((operation & 1) && messages.size() < 32) {
            auto requested_fd_count = input.byte() % (min(8uz, static_cast<size_t>(Core::LocalSocket::MAX_TRANSFER_FDS)) + 1);
            auto bytes = input.take(input.byte() % 65);
            IPC::MessageDataType payload;
            payload.append(bytes.data(), bytes.size());
            Vector<NonnullRefPtr<IPC::AutoCloseFileDescriptor>> fds;
            for (size_t i = 0; i < requested_fd_count; ++i) {
                auto file = Core::System::open("/dev/null"sv, O_RDONLY);
                if (file.is_error())
                    break;
                auto descriptor = adopt_ref(*new IPC::AutoCloseFileDescriptor(file.release_value()));
                expected_fds.append(descriptor->value());
                fds.append(move(descriptor));
            }
            auto fd_count = fds.size();
            IPC::SocketMessageHeader header {
                .type = IPC::SocketMessageHeader::Type::Payload,
                .payload_size = static_cast<u32>(bytes.size()),
                .fd_count = static_cast<u32>(fd_count),
            };
            messages.append({ header, Vector<u8>(bytes), expected_fds.size() });
            queue->enqueue_message(header, move(payload), move(fds));
        } else {
            auto maximum = 1 + input.byte();
            drain(maximum, input.byte());
        }
    }
    // Each full drain must advance at least one byte; queued data is bounded above.
    for (size_t step = 0; drain(4096, 0); ++step)
        VERIFY(step < 4096);
    VERIFY(sent_fd_count == expected_fds.size());
    FixedMemoryStream wire(sent_bytes.span());
    for (auto const& expected : messages) {
        IPC::SocketMessageHeader header {};
        MUST(wire.read_until_filled(Bytes { &header, sizeof(header) }));
        // Padding is unspecified; compare named fields, never the struct's bytes.
        VERIFY(header.type == expected.header.type);
        VERIFY(header.payload_size == expected.header.payload_size);
        VERIFY(header.fd_count == expected.header.fd_count);
        auto payload = MUST(ByteBuffer::create_uninitialized(header.payload_size));
        MUST(wire.read_until_filled(payload.bytes()));
        VERIFY(payload.bytes() == expected.payload.span());
    }
    VERIFY(wire.is_eof());
    return 0;
}
