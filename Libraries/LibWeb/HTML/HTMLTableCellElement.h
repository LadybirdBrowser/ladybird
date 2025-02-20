/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class HTMLTableCellElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLTableCellElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLTableCellElement);

public:
    virtual ~HTMLTableCellElement() override;

    WebIDL::UnsignedLong col_span() const;
    WebIDL::UnsignedLong row_span() const;

    WebIDL::ExceptionOr<void> set_col_span(WebIDL::UnsignedLong);
    WebIDL::ExceptionOr<void> set_row_span(WebIDL::UnsignedLong);

    WebIDL::Long cell_index() const;

    virtual Optional<ARIA::Role> default_role() const override;

private:
    HTMLTableCellElement(DOM::Document&, DOM::QualifiedName);

    virtual bool is_html_table_cell_element() const override { return true; }

    virtual void initialize(JS::Realm&) override;
    virtual bool is_presentational_hint(FlyString const&) const override;
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const override;
};

}

namespace Web::DOM {
template<>
inline bool Node::fast_is<HTML::HTMLTableCellElement>() const { return is_html_table_cell_element(); }
}
