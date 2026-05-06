/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibPaintServer/Resource/SharedArena.h>
#include <LibPaintServer/Types.h>

namespace PaintServer {

class RenderClientOfPaintServer;

class ArenaWriter {
    AK_MAKE_NONCOPYABLE(ArenaWriter);
    AK_MAKE_NONMOVABLE(ArenaWriter);

public:
    ArenaWriter();
    ~ArenaWriter();

    bool begin_packet();
    void abort_packet();
    void reset(RenderClientOfPaintServer&);
    void did_complete(PaintServer::ReleaseToken);
    void dump() const;

    ErrorOr<void> write(size_t offset, ReadonlyBytes);
    Optional<ReadonlyBytes> pending_slice(size_t offset, size_t size) const;
    Optional<PaintServer::SharedArena::Slice> finish_packet(RenderClientOfPaintServer&, PaintServer::ReleaseToken, ReadonlyBytes expected_prefix = {});
    PaintServer::ReleaseToken next_release_token() { return m_next_release_token++; }
    size_t arena_count() const { return m_arenas.size(); }

private:
    struct PendingPacketWrite {
        size_t arena_index { 0 };
        size_t start_offset { 0 };
        size_t packet_size { 0 };
    };

    static bool ensure_idle_arena_capacity(SharedArenaWithTokens&, size_t required_capacity);
    size_t desired_arena_capacity_for_payload(size_t payload_size);
    bool register_arena_if_needed(RenderClientOfPaintServer&, SharedArenaWithTokens&);
    bool relocate_pending_packet_write(size_t required_size);
    bool validate_slice_prefix(SharedArena::Slice const&, ReadonlyBytes expected_prefix) const;
    Optional<size_t> acquire_idle_arena(size_t required_capacity, Optional<size_t> excluded_arena_index = {});
    PaintServer::ArenaID next_arena_id() { return m_next_arena_id++; }

    Vector<NonnullOwnPtr<SharedArenaWithTokens>> m_arenas;
    Optional<PendingPacketWrite> m_pending_packet_write;
    PaintServer::ArenaID m_next_arena_id { 1 };
    PaintServer::ReleaseToken m_next_release_token { 1 };
    PaintServer::ReleaseToken m_last_completed_release_token { 0 };
    size_t m_active_arena_index { 0 };
    size_t m_recent_max_payload_size { 0 };
    bool m_force_register_arena { true };
};

}
