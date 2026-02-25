/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Layout/ReplacedBox.h>

namespace Web::Layout {

class VideoBox final : public ReplacedBox {
    GC_CELL(VideoBox, ReplacedBox);
    GC_DECLARE_ALLOCATOR(VideoBox);

public:
    HTML::HTMLVideoElement& dom_node();
    HTML::HTMLVideoElement const& dom_node() const;

    virtual bool can_have_children() const override;

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    VideoBox(DOM::Document&, DOM::Element&, GC::Ref<CSS::ComputedProperties>);
    virtual CSS::SizeWithAspectRatio natural_size() const override;
};

}
