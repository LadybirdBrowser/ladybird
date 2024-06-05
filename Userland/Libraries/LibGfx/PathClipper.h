/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Bitmap.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Path.h>
#include <LibGfx/WindingRule.h>

namespace Gfx {

struct ClipPath {
    Path path;
    WindingRule winding_rule;
};

class PathClipper {
public:
    static ErrorOr<PathClipper> create(Painter&, ClipPath const& clip_path);
    ErrorOr<void> apply_clip(Painter& painter);

private:
    PathClipper(RefPtr<Bitmap const> saved_clip_region, IntRect bounding_box, ClipPath const& clip_path)
        : m_saved_clip_region(saved_clip_region)
        , m_bounding_box(bounding_box)
        , m_clip_path(clip_path)
    {
    }

    RefPtr<Bitmap const> m_saved_clip_region;
    IntRect m_bounding_box;
    ClipPath const& m_clip_path;
};

class ScopedPathClip {
    AK_MAKE_NONMOVABLE(ScopedPathClip);
    AK_MAKE_NONCOPYABLE(ScopedPathClip);

public:
    ScopedPathClip(Painter& painter, Optional<ClipPath> const& clip_path)
        : m_painter(painter)
    {
        if (clip_path.has_value()) {
            auto clipper = PathClipper::create(painter, *clip_path);
            if (!clipper.is_error())
                m_path_clipper = clipper.release_value();
            else
                dbgln("Error: Failed to apply clip path: {}", clipper.error());
        }
    }

    ~ScopedPathClip()
    {
        if (m_path_clipper.has_value())
            m_path_clipper->apply_clip(m_painter).release_value_but_fixme_should_propagate_errors();
    }

private:
    Painter& m_painter;
    Optional<PathClipper> m_path_clipper;
};

}
