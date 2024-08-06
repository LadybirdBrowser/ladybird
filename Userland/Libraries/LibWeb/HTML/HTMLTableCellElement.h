/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class HTMLTableCellElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLTableCellElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLTableCellElement);

public:
    virtual ~HTMLTableCellElement() override;

    unsigned col_span() const;
    unsigned row_span() const;

    WebIDL::ExceptionOr<void> set_col_span(unsigned);
    WebIDL::ExceptionOr<void> set_row_span(unsigned);

    WebIDL::Long cell_index() const;

    virtual Optional<ARIA::Role> default_role() const override;

private:
    HTMLTableCellElement(DOM::Document&, DOM::QualifiedName);

    virtual bool is_html_table_cell_element() const override { return true; }

    virtual void initialize(JS::Realm&) override;
    virtual void apply_presentational_hints(CSS::StyleProperties&) const override;
};

}

namespace Web::DOM {
template<>
inline bool Node::fast_is<HTML::HTMLTableCellElement>() const { return is_html_table_cell_element(); }
}
