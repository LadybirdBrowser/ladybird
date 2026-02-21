/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLMediaElement.h>

namespace Web::HTML {

class HTMLAudioElement final : public HTMLMediaElement {
    WEB_PLATFORM_OBJECT(HTMLAudioElement, HTMLMediaElement);
    GC_DECLARE_ALLOCATOR(HTMLAudioElement);

public:
    virtual ~HTMLAudioElement() override;

    virtual void adjust_computed_style(CSS::ComputedProperties& style) override;

    Layout::AudioBox* layout_node();
    Layout::AudioBox const* layout_node() const;

    bool should_paint() const;

private:
    HTMLAudioElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;
};

}
