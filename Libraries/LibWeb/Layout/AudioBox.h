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
    LAYOUT_NODE(AudioBox, ReplacedBox);

public:
    AudioBox(DOM::Document&, DOM::Element&, NonnullRefPtr<CSS::ComputedValues const>);

    HTML::HTMLAudioElement& dom_node();
    HTML::HTMLAudioElement const& dom_node() const;

    virtual bool can_have_children() const override;

    virtual RefPtr<Painting::Paintable> create_paintable() const override;
};

}
