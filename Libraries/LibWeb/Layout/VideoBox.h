/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/ViewportClient.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/ReplacedBox.h>

namespace Web::Layout {

class VideoBox final
    : public ReplacedBox
    , public DOM::ViewportClient {
    GC_CELL(VideoBox, ReplacedBox);
    GC_DECLARE_ALLOCATOR(VideoBox);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    HTML::HTMLVideoElement& dom_node();
    HTML::HTMLVideoElement const& dom_node() const;

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    VideoBox(DOM::Document&, DOM::Element&, GC::Ref<CSS::ComputedProperties>);
    virtual CSS::SizeWithAspectRatio natural_size() const override;

    // ^Document::ViewportClient
    virtual void did_set_viewport_rect(CSSPixelRect const&) final;

    // ^JS::Cell
    virtual void finalize() override;
};

}
