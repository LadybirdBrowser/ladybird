/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class HTMLTableColElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLTableColElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLTableColElement);

public:
    virtual ~HTMLTableColElement() override;

    WebIDL::UnsignedLong span() const;
    void set_span(WebIDL::UnsignedLong);

private:
    HTMLTableColElement(DOM::Document&, DOM::QualifiedName);

    virtual bool is_html_table_col_element() const override { return true; }
    virtual bool is_presentational_hint(Utf16FlyString const&) const override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const override;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLTableColElement>() const { return is_html_table_col_element(); }

}
