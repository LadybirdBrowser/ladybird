/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/NavigableContainerViewport.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class NavigableContainerViewportPaintable final : public PaintableBox {
public:
    virtual bool is_navigable_container_viewport_paintable() const override { return true; }

    static NonnullRefPtr<NavigableContainerViewportPaintable> create(Layout::NavigableContainerViewport const&);
    virtual StringView class_name() const override { return "NavigableContainerViewportPaintable"sv; }

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

    auto const& navigable_container() const { return as<HTML::NavigableContainer>(*dom_node()); }

private:
    NavigableContainerViewportPaintable(Layout::NavigableContainerViewport const&);
};

template<>
inline bool Paintable::fast_is<NavigableContainerViewportPaintable>() const { return is_navigable_container_viewport_paintable(); }

}
