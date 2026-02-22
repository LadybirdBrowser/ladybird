/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/Layout/ReplacedBox.h>

namespace Web::Layout {

class AudioBox final : public ReplacedBox {
    GC_CELL(AudioBox, ReplacedBox);
    GC_DECLARE_ALLOCATOR(AudioBox);

public:
    HTML::HTMLAudioElement& dom_node();
    HTML::HTMLAudioElement const& dom_node() const;

    virtual bool can_have_children() const override;

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    // Treat the audio element as if it was not a replaced element, sizing based on its content.
    // Thus, it can fit to the shadow DOM controls, instead of having a hardcoded height.
    virtual bool has_auto_content_box_size() const override { return false; }
    AudioBox(DOM::Document&, DOM::Element&, GC::Ref<CSS::ComputedProperties>);
};

}
