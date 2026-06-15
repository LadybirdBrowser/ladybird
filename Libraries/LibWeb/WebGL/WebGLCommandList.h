/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebGL/WebGLCommands.h>

namespace Web::WebGL {

struct WebGLCommandHeader {
    WebGLCommandType type;
    u32 payload_size { 0 }; // command struct + inline data + trailing padding
};
static_assert(IsTriviallyCopyable<WebGLCommandHeader>);

class WEB_API WebGLCommandList {
public:
    static constexpr size_t command_alignment = 16;

    static constexpr u32 first_inline_data_offset(size_t command_size)
    {
        return static_cast<u32>(align_up_to(command_size, command_alignment));
    }

    static constexpr u32 next_inline_data_offset(WebGLDataSpan previous)
    {
        return static_cast<u32>(align_up_to(previous.offset + previous.size, command_alignment));
    }

    template<typename Command>
    void append(Command const& command, ReadonlyBytes inline_data = {})
    {
        append_bytes(Command::command_type, { &command, sizeof(command) }, inline_data);
    }

    void append_bytes(WebGLCommandType, ReadonlyBytes payload, ReadonlyBytes inline_data);

    template<typename Callback>
    static ErrorOr<void> for_each_command(ReadonlyBytes bytes, Callback&& callback)
    {
        size_t offset = 0;
        while (offset < bytes.size()) {
            if (bytes.size() - offset < sizeof(WebGLCommandHeader))
                return Error::from_string_literal("Truncated WebGL command header");
            WebGLCommandHeader header;
            __builtin_memcpy(&header, bytes.offset_pointer(offset), sizeof(header));
            if (to_underlying(header.type) >= webgl_command_type_count)
                return Error::from_string_literal("Invalid WebGL command type");
            if (header.payload_size > bytes.size() - offset - sizeof(header))
                return Error::from_string_literal("Truncated WebGL command payload");
            auto payload = bytes.slice(offset + sizeof(header), header.payload_size);
            TRY(visit_webgl_command_type(header.type, [&]<typename Command>() -> ErrorOr<void> {
                if (payload.size() < sizeof(Command))
                    return Error::from_string_literal("WebGL command payload too small");
                Command command;
                __builtin_memcpy(&command, payload.data(), sizeof(Command));
                return callback(command, payload);
            }));
            offset += sizeof(WebGLCommandHeader) + header.payload_size;
        }
        return {};
    }

    static ReadonlyBytes resolve_data_span(ReadonlyBytes payload, WebGLDataSpan span)
    {
        VERIFY(span.offset <= payload.size());
        VERIFY(span.size <= payload.size() - span.offset);
        return payload.slice(span.offset, span.size);
    }

    template<typename T>
    static Span<T const> resolve_typed_span(ReadonlyBytes payload, WebGLDataSpan span)
    {
        auto bytes = resolve_data_span(payload, span);
        VERIFY(reinterpret_cast<uintptr_t>(bytes.data()) % alignof(T) == 0);
        VERIFY(bytes.size() % sizeof(T) == 0);
        return Span<T const> { reinterpret_cast<T const*>(bytes.data()), bytes.size() / sizeof(T) };
    }

    static ReadonlyBytes resolve_string_span(ReadonlyBytes payload, WebGLDataSpan span)
    {
        auto bytes = resolve_data_span(payload, span);
        VERIFY(!bytes.is_empty());
        VERIFY(bytes[bytes.size() - 1] == 0);
        return bytes;
    }

    static void copy_data_span(ReadonlyBytes payload, WebGLDataSpan span, Bytes destination)
    {
        auto resolved = resolve_data_span(payload, span);
        VERIFY(resolved.size() <= destination.size());
        __builtin_memcpy(destination.data(), resolved.data(), resolved.size());
    }

    ReadonlyBytes bytes() const { return m_bytes; }
    ByteBuffer const& buffer() const { return m_bytes; }
    void clear_with_capacity() { m_bytes.set_size(0); }
    bool is_empty() const { return m_bytes.is_empty(); }
    size_t size_in_bytes() const { return m_bytes.size(); }

private:
    ByteBuffer m_bytes;
};

struct WebGLSyncCallHeader {
    WebGLSyncCallType type;
    u32 payload_size { 0 };
};
static_assert(IsTriviallyCopyable<WebGLSyncCallHeader>);

class WEB_API WebGLSyncCall {
public:
    template<typename Call>
    static ByteBuffer encode_request(typename Call::Request const& request, ReadonlyBytes inline_data = {})
    {
        return encode_request_bytes(Call::call_type, { &request, sizeof(request) }, inline_data);
    }

    template<typename Callback>
    static ErrorOr<ByteBuffer> dispatch_request(ReadonlyBytes bytes, Callback&& callback)
    {
        if (bytes.size() < sizeof(WebGLSyncCallHeader))
            return Error::from_string_literal("Truncated WebGL sync call header");
        WebGLSyncCallHeader header;
        __builtin_memcpy(&header, bytes.data(), sizeof(header));
        if (to_underlying(header.type) >= webgl_sync_call_type_count)
            return Error::from_string_literal("Invalid WebGL sync call type");
        if (header.payload_size != bytes.size() - sizeof(header))
            return Error::from_string_literal("Truncated WebGL sync call payload");
        auto payload = bytes.slice(sizeof(header), header.payload_size);
        return visit_webgl_sync_call_type(header.type, [&]<typename Call>() -> ErrorOr<ByteBuffer> {
            if (payload.size() < sizeof(typename Call::Request))
                return Error::from_string_literal("WebGL sync call payload too small");
            typename Call::Request request;
            __builtin_memcpy(&request, payload.data(), sizeof(request));
            return callback.template operator()<Call>(request, payload);
        });
    }

    template<typename Reply>
    static ByteBuffer encode_reply(Reply const& reply, ReadonlyBytes inline_data = {}, ReadonlyBytes more_inline_data = {})
    {
        return encode_reply_bytes({ &reply, sizeof(reply) }, inline_data, more_inline_data);
    }

    template<typename Reply>
    static Reply decode_reply(ReadonlyBytes bytes)
    {
        VERIFY(bytes.size() >= sizeof(Reply));
        Reply reply;
        __builtin_memcpy(&reply, bytes.data(), sizeof(reply));
        return reply;
    }

private:
    static ByteBuffer encode_request_bytes(WebGLSyncCallType, ReadonlyBytes request, ReadonlyBytes inline_data);
    static ByteBuffer encode_reply_bytes(ReadonlyBytes reply, ReadonlyBytes inline_data, ReadonlyBytes more_inline_data);
};

}
