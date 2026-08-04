/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLEmbedElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLEmbedElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLEmbedElement);

public:
    virtual ~HTMLEmbedElement() override;

private:
    HTMLEmbedElement(DOM::Document&, DOM::QualifiedName);

    virtual bool is_html_embed_element() const override { return true; }
    virtual bool is_presentational_hint(Utf16FlyString const&) const override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const override;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLEmbedElement>() const { return is_html_embed_element(); }

}
