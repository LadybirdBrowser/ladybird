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
    LAYOUT_NODE(NavigableContainerViewport, ReplacedBox);

public:
    NavigableContainerViewport(DOM::Document&, HTML::NavigableContainer&, CSS::ComputedProperties const&);
    virtual ~NavigableContainerViewport() override;

    [[nodiscard]] HTML::NavigableContainer const& dom_node() const { return as<HTML::NavigableContainer>(*ReplacedBox::dom_node()); }
    [[nodiscard]] HTML::NavigableContainer& dom_node() { return as<HTML::NavigableContainer>(*ReplacedBox::dom_node()); }

    virtual RefPtr<Painting::Paintable> create_paintable() const override;

private:
    virtual CSS::SizeWithAspectRatio natural_size() const override;
    virtual void did_set_content_size() override;
};

}
