/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibPaintServer/Debug.h>
#include <PaintServer/PresentationBufferState.h>

namespace PaintServer {

static bool image_id_is_reserved(HashMap<u64, u64> const& present_id_to_image_id, u64 image_id)
{
    for (auto const& it : present_id_to_image_id) {
        if (it.value == image_id)
            return true;
    }
    return false;
}

void PresentationBufferState::clear_surface(SurfaceID surface_id)
{
    auto maybe_pool = m_presentation_buffer_pools.get(surface_id);
    if (maybe_pool.has_value()) {
        Vector<u64> present_ids_to_remove;
        present_ids_to_remove.ensure_capacity(m_present_id_to_image_id.size());
        for (auto const& it : m_present_id_to_image_id) {
            if (maybe_pool->contains_slow(it.value))
                present_ids_to_remove.append(it.key);
        }

        for (u64 present_id : present_ids_to_remove)
            m_present_id_to_image_id.remove(present_id);

        for (u64 image_id : *maybe_pool)
            m_image_frame_sizes.remove(image_id);
    }

    m_presentation_buffer_pools.remove(surface_id);
    m_presentation_buffer_next_index.remove(surface_id);
    m_image_frame_sizes.remove(surface_id);
}

void PresentationBufferState::add_surface_buffer(SurfaceID surface_id, ImageID image_id)
{
    auto& pool = m_presentation_buffer_pools.ensure(surface_id, [] { return Vector<ImageID> {}; });
    if (pool.contains_slow(image_id))
        return;
    pool.append(image_id);
    if (!m_presentation_buffer_next_index.contains(surface_id))
        m_presentation_buffer_next_index.set(surface_id, 0);
}

Optional<u64> PresentationBufferState::reserve_next_buffer(SurfaceID surface_id, u64 present_id)
{
    auto maybe_pool = m_presentation_buffer_pools.get(surface_id);
    if (!maybe_pool.has_value() || maybe_pool->is_empty())
        return {};

    size_t index = 0;
    if (auto maybe_index = m_presentation_buffer_next_index.get(surface_id); maybe_index.has_value())
        index = maybe_index.value();

    size_t pool_size = maybe_pool->size();
    for (size_t attempt = 0; attempt < pool_size; ++attempt) {
        size_t candidate_index = (index + attempt) % pool_size;
        u64 candidate_image_id = maybe_pool->at(candidate_index);
        if (image_id_is_reserved(m_present_id_to_image_id, candidate_image_id))
            continue;

        m_presentation_buffer_next_index.set(surface_id, (candidate_index + 1) % pool_size);
        m_present_id_to_image_id.set(present_id, candidate_image_id);
        return candidate_image_id;
    }

    if (is_logging_enabled()) {
        auto once_key = ByteString::formatted("presentation-buffer-exhausted-surface-{}", surface_id);
        dbgonce(100, once_key, "PresentationBufferState: no free presentation buffer surface={} pool_size={} in_flight_count={} next_index={}",
            surface_id,
            pool_size,
            m_present_id_to_image_id.size(),
            index);
    }

    return {};
}

void PresentationBufferState::release_submit_reservation(u64 present_id, Optional<u64> reserved_image_id)
{
    auto maybe_image_id = m_present_id_to_image_id.get(present_id);
    if (!maybe_image_id.has_value())
        return;

    if (reserved_image_id.has_value() && maybe_image_id.value() != reserved_image_id.value())
        return;

    m_present_id_to_image_id.remove(present_id);
}

PresentationBufferState::PresentReleaseResult PresentationBufferState::did_present_or_released(u64 present_id)
{
    PresentReleaseResult result;

    auto maybe_image_id = m_present_id_to_image_id.get(present_id);
    if (!maybe_image_id.has_value())
        return result;

    result.image_id = maybe_image_id.value();
    result.had_mapping = true;
    m_present_id_to_image_id.remove(present_id);
    return result;
}

void PresentationBufferState::stamp_image(ImageID image_id, Gfx::IntSize frame_size)
{
    m_image_frame_sizes.set(image_id, frame_size);
}

Optional<Gfx::IntSize> PresentationBufferState::stamped_frame_size_for_image(ImageID image_id) const
{
    return m_image_frame_sizes.get(image_id);
}

}
