/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::Painting {

class NavigableContainerViewportPaintable final : public Paintable {
public:
    virtual bool is_navigable_container_viewport_paintable() const override { return true; }

    static NonnullRefPtr<NavigableContainerViewportPaintable> create(Layout::Box const&);
    virtual StringView class_name() const override { return "NavigableContainerViewportPaintable"sv; }

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

    auto const& navigable_container() const { return as<HTML::NavigableContainer>(*dom_node()); }

private:
    NavigableContainerViewportPaintable(Layout::Box const&);
};

template<>
inline bool Paintable::fast_is<NavigableContainerViewportPaintable>() const { return is_navigable_container_viewport_paintable(); }

}
