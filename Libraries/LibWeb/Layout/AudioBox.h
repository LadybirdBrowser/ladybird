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
    JS_CELL(AudioBox, ReplacedBox);
    JS_DECLARE_ALLOCATOR(AudioBox);

public:
    HTML::HTMLAudioElement& dom_node();
    HTML::HTMLAudioElement const& dom_node() const;

    virtual JS::GCPtr<Painting::Paintable> create_paintable() const override;

private:
    AudioBox(DOM::Document&, DOM::Element&, CSS::StyleProperties);
};

}
