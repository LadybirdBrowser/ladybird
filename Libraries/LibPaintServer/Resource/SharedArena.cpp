/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/StdLibExtras.h>
#include <LibPaintServer/Resource/SharedArena.h>

namespace PaintServer {

bool SharedArena::ensure_capacity(size_t size)
{
    if (m_buffer.is_valid() && m_capacity >= size)
        return true;

    ErrorOr<Core::AnonymousBuffer> arena_or_error = Core::AnonymousBuffer::create_with_size(size);
    if (arena_or_error.is_error()) {
        dbgln("SharedArena: create_with_size failed size={} error={}", size, arena_or_error.error());
        return false;
    }

    m_buffer = arena_or_error.release_value();
    m_capacity = size;
    m_bytes_used = 0;
    return true;
}

bool SharedArena::rewind(size_t mark)
{
    if (mark > m_bytes_used)
        return false;
    m_bytes_used = mark;
    return true;
}

Optional<SharedArena::Allocation> SharedArena::allocate(size_t size)
{
    if (size == 0)
        return {};

    size_t const allocation_offset = m_bytes_used;
    auto bytes = allocate_bytes(size);
    if (!bytes.has_value())
        return {};

    Slice slice {
        .arena_id = m_arena_id,
        .offset = allocation_offset,
        .size = size,
    };

    return Allocation {
        .bytes = bytes.release_value(),
        .slice = slice,
    };
}

Optional<SharedArena::Slice> SharedArena::make_slice(size_t offset, size_t size) const
{
    if (!m_buffer.is_valid())
        return {};
    if (m_arena_id == 0)
        return {};
    if (size == 0)
        return {};
    if (offset > m_capacity)
        return {};
    if (size > m_capacity - offset)
        return {};

    return Slice {
        .arena_id = m_arena_id,
        .offset = offset,
        .size = size,
    };
}

Optional<Bytes> SharedArena::bytes_at(size_t offset, size_t size)
{
    if (!m_buffer.is_valid() || offset > m_capacity || size > m_capacity - offset)
        return {};

    u8* start = m_buffer.data<u8>() + offset;
    return Bytes { start, size };
}

Optional<ReadonlyBytes> SharedArena::readonly_bytes_at(size_t offset, size_t size) const
{
    if (!m_buffer.is_valid() || offset > m_capacity || size > m_capacity - offset)
        return {};

    u8 const* start = m_buffer.data<u8>() + offset;
    return ReadonlyBytes { start, size };
}

Optional<Bytes> SharedArena::allocate_bytes(size_t size)
{
    if (!m_buffer.is_valid() || size > m_capacity - m_bytes_used)
        return {};

    u8* start = m_buffer.data<u8>() + m_bytes_used;
    m_bytes_used += size;
    return Bytes { start, size };
}

ErrorOr<IPC::File> SharedArena::clone_file() const
{
    if (!m_buffer.is_valid())
        return Error::from_string_literal("SharedArena is not valid");

    auto cloned = IPC::File::clone_fd(m_buffer.fd());
    if (cloned.is_error())
        AK::warnln("SharedArena: clone_fd failed fd={} error={}", m_buffer.fd(), cloned.error());
    return cloned;
}

void SharedArena::clear()
{
    m_buffer = {};
    m_capacity = 0;
    m_bytes_used = 0;
}

void SharedArenaWithTokens::reset_allocator_state()
{
    m_in_flight_tokens.clear();
    m_in_flight_head = 0;
    m_last_completed_token = 0;
    m_arena.reset();
}

bool SharedArenaWithTokens::is_idle() const
{
    return m_in_flight_head >= m_in_flight_tokens.size();
}

size_t SharedArenaWithTokens::in_flight_count() const
{
    if (is_idle())
        return 0;
    return m_in_flight_tokens.size() - m_in_flight_head;
}

Optional<ReleaseToken> SharedArenaWithTokens::oldest_in_flight_token() const
{
    if (is_idle())
        return {};
    return m_in_flight_tokens[m_in_flight_head];
}

Optional<ReleaseToken> SharedArenaWithTokens::newest_in_flight_token() const
{
    if (is_idle())
        return {};
    return m_in_flight_tokens.last();
}

void SharedArenaWithTokens::note_submission(ReleaseToken release_token)
{
    reclaim_completed(false);
    m_in_flight_tokens.append(release_token);
}

void SharedArenaWithTokens::did_complete(ReleaseToken completed_token)
{
    if (completed_token < m_last_completed_token)
        return;
    m_last_completed_token = completed_token;
    reclaim_completed(true);
}

void SharedArenaWithTokens::reclaim_completed(bool reset_arena_when_idle)
{
    while (m_in_flight_head < m_in_flight_tokens.size()) {
        if (m_in_flight_tokens[m_in_flight_head] > m_last_completed_token)
            break;
        ++m_in_flight_head;
    }

    if (is_idle()) {
        m_in_flight_tokens.clear();
        m_in_flight_head = 0;
        if (reset_arena_when_idle)
            m_arena.reset();
        return;
    }

    if (m_in_flight_head > 64 && m_in_flight_head * 2 > m_in_flight_tokens.size()) {
        m_in_flight_tokens.remove(0, m_in_flight_head);
        m_in_flight_head = 0;
    }
}

Optional<SharedArena::Allocation> SharedArenaWithTokens::allocate(size_t size, ReleaseToken release_token)
{
    reclaim_completed(true);

    auto allocation = m_arena.allocate(size);
    if (!allocation.has_value())
        return {};

    m_in_flight_tokens.append(release_token);
    return allocation;
}

}
