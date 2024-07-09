/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLTableCaptionElement.h>
#include <LibWeb/HTML/HTMLTableRowElement.h>
#include <LibWeb/HTML/HTMLTableSectionElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class HTMLTableElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLTableElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLTableElement);

public:
    virtual ~HTMLTableElement() override;

    JS::GCPtr<HTMLTableCaptionElement> caption();
    WebIDL::ExceptionOr<void> set_caption(HTMLTableCaptionElement*);
    JS::NonnullGCPtr<HTMLTableCaptionElement> create_caption();
    void delete_caption();

    JS::GCPtr<HTMLTableSectionElement> t_head();
    WebIDL::ExceptionOr<void> set_t_head(HTMLTableSectionElement* thead);
    JS::NonnullGCPtr<HTMLTableSectionElement> create_t_head();
    void delete_t_head();

    JS::GCPtr<HTMLTableSectionElement> t_foot();
    WebIDL::ExceptionOr<void> set_t_foot(HTMLTableSectionElement* tfoot);
    JS::NonnullGCPtr<HTMLTableSectionElement> create_t_foot();
    void delete_t_foot();

    JS::NonnullGCPtr<DOM::HTMLCollection> t_bodies();
    JS::NonnullGCPtr<HTMLTableSectionElement> create_t_body();

    JS::NonnullGCPtr<DOM::HTMLCollection> rows();
    WebIDL::ExceptionOr<JS::NonnullGCPtr<HTMLTableRowElement>> insert_row(WebIDL::Long index);
    WebIDL::ExceptionOr<void> delete_row(WebIDL::Long index);

    // https://www.w3.org/TR/html-aria/#el-table
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::table; }

    unsigned border() const;
    unsigned padding() const;

private:
    HTMLTableElement(DOM::Document&, DOM::QualifiedName);

    virtual bool is_html_table_element() const override { return true; }

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void apply_presentational_hints(CSS::StyleProperties&) const override;
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value) override;

    JS::GCPtr<DOM::HTMLCollection> mutable m_rows;
    JS::GCPtr<DOM::HTMLCollection> mutable m_t_bodies;
    unsigned m_padding { 1 };
};

}

namespace Web::DOM {
template<>
inline bool Node::fast_is<HTML::HTMLTableElement>() const { return is_html_table_element(); }
}
