/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <AK/StdLibExtras.h>
#include <LibPaintServer/ArenaWriter.h>
#include <LibPaintServer/Debug.h>
#include <LibPaintServer/Policy.h>
#include <LibPaintServer/RenderClientOfPaintServer.h>
#include <string.h>

namespace PaintServer {

static PaintServer::ArenaID s_next_arena_id = 1;

static char const* packet_kind_name(ArenaPacketKind kind)
{
    switch (kind) {
    case ArenaPacketKind::Frame:
        return "Frame";
    case ArenaPacketKind::Resource:
        return "Resource";
    case ArenaPacketKind::Canvas:
        return "Canvas";
    }
    return "Unknown";
}

ArenaWriter::ArenaWriter()
{
    m_arenas.append(make<SharedArenaWithTokens>(next_arena_id()));
    m_active_arena_index = 0;
}

ArenaWriter::~ArenaWriter() = default;

void ArenaWriter::reset(RenderClientOfPaintServer& render_client)
{
    m_pending_packet_write.clear();

    for (auto& arena : m_arenas) {
        if (arena->is_registered())
            render_client.async_unregister_arena(arena->arena().arena_id());
    }

    m_force_register_arena = true;
    m_arenas.clear();
    m_arenas.append(make<SharedArenaWithTokens>(next_arena_id()));
    m_active_arena_index = 0;
    m_recent_max_payload_size = 0;
    m_last_completed_release_token = 0;
}

void ArenaWriter::did_complete(PaintServer::ReleaseToken release_token)
{
    m_last_completed_release_token = release_token;

    for (auto& arena : m_arenas)
        arena->did_complete(release_token);
}

bool ArenaWriter::begin_packet()
{
    if (m_arenas.is_empty()) {
        m_arenas.append(make<SharedArenaWithTokens>(next_arena_id()));
        m_active_arena_index = 0;
    } else if (m_active_arena_index >= m_arenas.size()) {
        m_active_arena_index = 0;
    }

    auto& active_arena = *m_arenas[m_active_arena_index];
    active_arena.did_complete(m_last_completed_release_token);

    if (!active_arena.arena().is_valid()) {
        size_t const desired_capacity = desired_arena_capacity_for_payload(0);
        if (!active_arena.is_idle()) {
            auto idle_arena_index = acquire_idle_arena(desired_capacity, {});
            if (!idle_arena_index.has_value())
                return false;
            m_active_arena_index = idle_arena_index.release_value();
        } else if (!ensure_idle_arena_capacity(active_arena, desired_capacity)) {
            return false;
        }
    }

    m_pending_packet_write = PendingPacketWrite {
        .arena_index = m_active_arena_index,
        .start_offset = m_arenas[m_active_arena_index]->arena().bytes_used(),
        .packet_size = 0,
    };
    return true;
}

void ArenaWriter::abort_packet()
{
    if (!m_pending_packet_write.has_value())
        return;

    auto& pending_write = m_pending_packet_write.value();
    auto& arena = *m_arenas[pending_write.arena_index];
    [[maybe_unused]] bool rewound = arena.arena().rewind(pending_write.start_offset);
    VERIFY(rewound);
    m_pending_packet_write.clear();
}

ErrorOr<void> ArenaWriter::write(size_t offset, ReadonlyBytes bytes)
{
    if (!m_pending_packet_write.has_value())
        return Error::from_string_literal("ArenaWriter has no active packet write");

    auto& pending_write = m_pending_packet_write.value();
    if (offset > NumericLimits<size_t>::max() - bytes.size())
        return Error::from_string_literal("ArenaWriter packet write overflows size");

    size_t const required_size = offset + bytes.size();
    auto& arena = *m_arenas[pending_write.arena_index];
    if (!arena.arena().is_valid() || arena.arena().capacity() - pending_write.start_offset < required_size) {
        if (!relocate_pending_packet_write(required_size))
            return Error::from_string_literal("ArenaWriter failed to relocate packet write");
    }

    auto& destination_arena = *m_arenas[pending_write.arena_index];
    if (required_size > pending_write.packet_size) {
        if (destination_arena.arena().bytes_used() != pending_write.start_offset + pending_write.packet_size)
            return Error::from_string_literal("ArenaWriter packet write lost tail position");

        size_t const gap_size = offset > pending_write.packet_size ? offset - pending_write.packet_size : 0;
        auto tail_bytes = destination_arena.arena().allocate_bytes(required_size - pending_write.packet_size);
        if (!tail_bytes.has_value())
            return Error::from_string_literal("ArenaWriter failed to extend packet write");

        if (gap_size != 0)
            __builtin_memset(tail_bytes->data(), 0, gap_size);

        pending_write.packet_size = required_size;
    }

    auto destination_bytes = destination_arena.arena().bytes_at(pending_write.start_offset + offset, bytes.size());
    if (!destination_bytes.has_value())
        return Error::from_string_literal("ArenaWriter failed to access packet write range");

    bytes.copy_to(destination_bytes.release_value());
    return {};
}

Optional<ReadonlyBytes> ArenaWriter::pending_slice(size_t offset, size_t size) const
{
    if (!m_pending_packet_write.has_value())
        return {};

    auto const& pending_write = m_pending_packet_write.value();
    if (offset > pending_write.packet_size || size > pending_write.packet_size - offset)
        return {};

    if (pending_write.arena_index >= m_arenas.size())
        return {};

    auto const& arena = *m_arenas[pending_write.arena_index];
    return arena.arena().readonly_bytes_at(pending_write.start_offset + offset, size);
}

Optional<SharedArena::Slice> ArenaWriter::finish_packet(RenderClientOfPaintServer& render_client, PaintServer::ReleaseToken release_token, ReadonlyBytes expected_prefix)
{
    if (!m_pending_packet_write.has_value())
        return {};

    auto const& pending_write = m_pending_packet_write.value();
    if (pending_write.packet_size == 0)
        return {};

    auto& arena = *m_arenas[pending_write.arena_index];
    auto slice = arena.arena().make_slice(pending_write.start_offset, pending_write.packet_size);
    if (!slice.has_value())
        return {};
    if (!validate_slice_prefix(slice.value(), expected_prefix))
        return {};

    if (is_logging_enabled(LOG_INGRESS)) {
        ArenaPacketKind kind = ArenaPacketKind::Frame;
        if (!expected_prefix.is_empty())
            kind = static_cast<ArenaPacketKind>(expected_prefix[0]);
        dbgln("ArenaWriter: finish_packet release_token={} kind={} arena_id={} offset={} size={} arena_bytes_used={} arena_capacity={}",
            release_token,
            packet_kind_name(kind),
            slice->arena_id,
            slice->offset,
            slice->size,
            arena.arena().bytes_used(),
            arena.arena().capacity());
    }

    if (!register_arena_if_needed(render_client, arena))
        return {};
    m_recent_max_payload_size = max(m_recent_max_payload_size, pending_write.packet_size);

    arena.note_submission(release_token);
    m_active_arena_index = pending_write.arena_index;
    m_pending_packet_write.clear();
    return slice;
}

bool ArenaWriter::validate_slice_prefix(SharedArena::Slice const& slice, ReadonlyBytes expected_prefix) const
{
    if (expected_prefix.is_empty())
        return true;
    if (slice.size < expected_prefix.size())
        return false;

    for (auto const& arena : m_arenas) {
        if (arena->arena().arena_id() != slice.arena_id)
            continue;

        auto actual_prefix = arena->arena().readonly_bytes_at(slice.offset, expected_prefix.size());
        if (!actual_prefix.has_value())
            return false;
        return memcmp(actual_prefix->data(), expected_prefix.data(), expected_prefix.size()) == 0;
    }

    return false;
}

bool ArenaWriter::relocate_pending_packet_write(size_t required_size)
{
    VERIFY(m_pending_packet_write.has_value());
    auto& pending_write = m_pending_packet_write.value();
    size_t const desired_capacity = desired_arena_capacity_for_payload(max(pending_write.packet_size, required_size));

    auto destination_arena_index = acquire_idle_arena(desired_capacity, pending_write.arena_index);
    if (!destination_arena_index.has_value())
        return false;

    auto& source_arena = *m_arenas[pending_write.arena_index];
    auto& destination_arena = *m_arenas[destination_arena_index.value()];

    if (pending_write.packet_size != 0) {
        auto source_bytes = source_arena.arena().readonly_bytes_at(pending_write.start_offset, pending_write.packet_size);
        if (!source_bytes.has_value())
            return false;

        auto destination_bytes = destination_arena.arena().allocate_bytes(pending_write.packet_size);
        if (!destination_bytes.has_value()) {
            destination_arena.arena().reset();
            return false;
        }
        source_bytes->copy_to(destination_bytes.release_value());
    }

    bool rewound = source_arena.arena().rewind(pending_write.start_offset);
    VERIFY(rewound);

    pending_write.arena_index = destination_arena_index.release_value();
    pending_write.start_offset = 0;
    m_active_arena_index = pending_write.arena_index;
    return true;
}

Optional<size_t> ArenaWriter::acquire_idle_arena(size_t required_capacity, Optional<size_t> excluded_arena_index)
{
    if (m_arenas.is_empty()) {
        m_arenas.append(make<SharedArenaWithTokens>(next_arena_id()));
        m_active_arena_index = 0;
    }

    for (size_t offset = 0; offset < m_arenas.size(); ++offset) {
        size_t const arena_index = (m_active_arena_index + offset) % m_arenas.size();
        if (excluded_arena_index.has_value() && arena_index == excluded_arena_index.value())
            continue;

        auto& arena = *m_arenas[arena_index];
        arena.did_complete(m_last_completed_release_token);
        if (!arena.is_idle())
            continue;
        if (!ensure_idle_arena_capacity(arena, required_capacity))
            continue;
        return arena_index;
    }

    auto next_arena = make<SharedArenaWithTokens>(next_arena_id());
    if (!ensure_idle_arena_capacity(*next_arena, required_capacity))
        return {};
    if (is_logging_enabled()) {
        dbgln("ArenaWriter: required_capacity={} forced new arena_id={}, total_arenas={}. new_capacity={}",
            required_capacity,
            next_arena->arena().arena_id(),
            m_arenas.size() + 1,
            next_arena->arena().capacity());
    }
    m_arenas.append(move(next_arena));
    return m_arenas.size() - 1;
}

bool ArenaWriter::ensure_idle_arena_capacity(SharedArenaWithTokens& arena, size_t required_capacity)
{
    VERIFY(arena.is_idle());

    if (!arena.arena().is_valid() || arena.arena().capacity() < required_capacity) {
        if (!arena.arena().ensure_capacity(required_capacity))
            return false;
        arena.reset_allocator_state();
        arena.set_registered(false);
        return true;
    }

    arena.arena().reset();
    return true;
}

size_t ArenaWriter::desired_arena_capacity_for_payload(size_t payload_size)
{
    m_recent_max_payload_size = max(m_recent_max_payload_size, payload_size);
    return max(MIN_SUBMIT_ARENA_CAPACITY, m_recent_max_payload_size * SUBMIT_ARENA_HEADROOM_FACTOR);
}

bool ArenaWriter::register_arena_if_needed(RenderClientOfPaintServer& render_client, SharedArenaWithTokens& arena)
{
    if (arena.is_registered() && !m_force_register_arena)
        return true;

    auto cloned_fd = arena.arena().clone_file();
    if (cloned_fd.is_error()) {
        dbgln("ArenaWriter: failed to clone shared arena fd arena_id={} error={}", arena.arena().arena_id(), cloned_fd.error());
        return false;
    }

    render_client.async_register_arena(arena.arena().arena_id(), cloned_fd.release_value(), arena.arena().capacity());
    arena.set_registered(true);
    m_force_register_arena = false;
    return true;
}

void ArenaWriter::dump() const
{
    size_t max_capacity = 0;
    for (size_t arena_index = 0; arena_index < m_arenas.size(); ++arena_index) {
        max_capacity = AK::max(m_arenas[arena_index]->arena().capacity(), max_capacity);
    }
    dbgln("ArenaWriter dump: arenas={} next_submit_arena_id={} recent_max_payload_size={} capacity={}",
        m_arenas.size(),
        s_next_arena_id,
        m_recent_max_payload_size,
        max_capacity);
}

}
