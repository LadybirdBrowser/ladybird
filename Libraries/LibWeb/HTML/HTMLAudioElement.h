/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLMediaElement.h>

namespace Web::HTML {

class HTMLAudioElement final : public HTMLMediaElement {
    WEB_WRAPPABLE(HTMLAudioElement, HTMLMediaElement);
    GC_DECLARE_ALLOCATOR(HTMLAudioElement);

public:
    virtual ~HTMLAudioElement() override;

    bool should_paint() const;

private:
    HTMLAudioElement(DOM::Document&, DOM::QualifiedName);
    virtual RefPtr<Layout::Node> create_layout_node(CSS::LayoutStyle) override;
};

}
