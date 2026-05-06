/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibPaintServer/Compositor/DrawCommands.h>

namespace PaintServer {

class Cursor {
public:
    explicit Cursor(ReadonlyBytes payload)
        : m_payload(payload)
    {
    }
    bool is_eof() const { return m_offset >= m_payload.size(); }
    ErrorOr<Optional<DrawCommandView>> next();

private:
    size_t m_offset { 0 };
    ReadonlyBytes m_payload;
};

class DrawListView {
public:
    DrawListView() = default;
    explicit DrawListView(ReadonlyBytes payload)
        : m_payload(payload)
    {
    }
    bool operator==(DrawListView const& other) const { return bytes() == other.bytes(); }

    ReadonlyBytes bytes() const { return m_payload; }
    size_t size() const { return m_payload.size(); }
    bool is_empty() const { return m_payload.is_empty(); }
    Cursor cursor() const { return Cursor(bytes()); }

private:
    ReadonlyBytes m_payload;
};

class DrawList {
public:
    static ErrorOr<DrawList> copy(ReadonlyBytes payload);

    DrawList() = default;
    bool operator==(DrawList const&) const = default;

    DrawListView view() const { return DrawListView(bytes()); }
    ReadonlyBytes bytes() const { return m_payload.bytes(); }
    size_t size() const { return m_payload.size(); }
    bool is_empty() const { return m_payload.is_empty(); }
    void clear() { m_payload.clear(); }
    Cursor cursor() const { return Cursor(bytes()); }

    ErrorOr<void> try_append_command(ReadonlyBytes command_bytes);
    ErrorOr<Vector<DrawCommandView>> scan_commands() const;

private:
    ByteBuffer m_payload;
};

u64 hash_draw_list_payload(ReadonlyBytes bytes);

ErrorOr<CommandType> decode_draw_list_command_type(ReadonlyBytes payload);

}
