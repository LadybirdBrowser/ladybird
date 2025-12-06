/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/ReplacedBox.h>

namespace Web::Layout {

class NavigableContainerViewport final : public ReplacedBox {
    GC_CELL(NavigableContainerViewport, ReplacedBox);
    GC_DECLARE_ALLOCATOR(NavigableContainerViewport);

public:
    NavigableContainerViewport(DOM::Document&, HTML::NavigableContainer&, GC::Ref<CSS::ComputedProperties>);
    virtual ~NavigableContainerViewport() override;

    [[nodiscard]] HTML::NavigableContainer const& dom_node() const { return as<HTML::NavigableContainer>(*ReplacedBox::dom_node()); }
    [[nodiscard]] HTML::NavigableContainer& dom_node() { return as<HTML::NavigableContainer>(*ReplacedBox::dom_node()); }

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

protected:
    virtual Optional<CSSPixels> compute_natural_width() const override;
    virtual Optional<CSSPixels> compute_natural_height() const override;
    virtual Optional<CSSPixelFraction> compute_natural_aspect_ratio() const override;

private:
    virtual void did_set_content_size() override;
};

}
