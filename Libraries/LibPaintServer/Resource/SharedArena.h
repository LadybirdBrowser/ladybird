/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/File.h>
#include <LibPaintServer/Types.h>

namespace PaintServer {

class SharedArena {
    AK_MAKE_NONCOPYABLE(SharedArena);
    AK_MAKE_DEFAULT_MOVABLE(SharedArena);

public:
    struct Slice {
        ArenaID arena_id { 0 };
        size_t offset { 0 };
        size_t size { 0 };

        bool is_valid() const { return size > 0; }
    };

    struct Allocation {
        Bytes bytes;
        Slice slice;
    };

    explicit SharedArena(ArenaID arena_id = 0)
        : m_arena_id(arena_id)
    {
    }
    ~SharedArena() = default;

    bool ensure_capacity(size_t size);
    bool is_valid() const { return m_buffer.is_valid(); }
    u8* data() { return m_buffer.data<u8>(); }
    u8 const* data() const { return m_buffer.data<u8>(); }
    size_t capacity() const { return m_capacity; }
    size_t bytes_used() const { return m_bytes_used; }
    bool rewind(size_t mark);
    void reset() { m_bytes_used = 0; }
    void set_arena_id(ArenaID arena_id) { m_arena_id = arena_id; }
    ArenaID arena_id() const { return m_arena_id; }

    Optional<Allocation> allocate(size_t size);
    Optional<Slice> make_slice(size_t offset, size_t size) const;
    Optional<Bytes> bytes_at(size_t offset, size_t size);
    Optional<ReadonlyBytes> readonly_bytes_at(size_t offset, size_t size) const;
    Optional<Bytes> allocate_bytes(size_t size);
    ErrorOr<IPC::File> clone_file() const;

    void clear();

private:
    Core::AnonymousBuffer m_buffer;
    ArenaID m_arena_id { 0 };
    size_t m_capacity { 0 };
    size_t m_bytes_used { 0 };
};

class SharedArenaWithTokens {
    AK_MAKE_NONCOPYABLE(SharedArenaWithTokens);
    AK_MAKE_DEFAULT_MOVABLE(SharedArenaWithTokens);

public:
    explicit SharedArenaWithTokens(ArenaID id = 0)
        : m_arena(id)
    {
    }
    ~SharedArenaWithTokens() = default;

    SharedArena& arena() { return m_arena; }
    SharedArena const& arena() const { return m_arena; }

    bool is_registered() const { return m_is_registered; }
    void set_registered(bool is_registered) { m_is_registered = is_registered; }
    void reset_allocator_state();
    bool is_idle() const;
    size_t in_flight_count() const;
    ReleaseToken last_completed_token() const { return m_last_completed_token; }
    Optional<ReleaseToken> oldest_in_flight_token() const;
    Optional<ReleaseToken> newest_in_flight_token() const;
    void note_submission(ReleaseToken release_token);
    void did_complete(ReleaseToken completed_token);
    Optional<SharedArena::Allocation> allocate(size_t size, ReleaseToken release_token);

private:
    void reclaim_completed(bool reset_arena_when_idle);

    SharedArena m_arena;
    bool m_is_registered { false };
    Vector<ReleaseToken> m_in_flight_tokens;
    size_t m_in_flight_head { 0 };
    ReleaseToken m_last_completed_token { 0 };
};

}
