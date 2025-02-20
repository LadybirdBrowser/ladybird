/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
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
    GC_DECLARE_ALLOCATOR(HTMLTableElement);

public:
    virtual ~HTMLTableElement() override;

    GC::Ptr<HTMLTableCaptionElement> caption();
    WebIDL::ExceptionOr<void> set_caption(HTMLTableCaptionElement*);
    GC::Ref<HTMLTableCaptionElement> create_caption();
    void delete_caption();

    GC::Ptr<HTMLTableSectionElement> t_head();
    WebIDL::ExceptionOr<void> set_t_head(HTMLTableSectionElement* thead);
    GC::Ref<HTMLTableSectionElement> create_t_head();
    void delete_t_head();

    GC::Ptr<HTMLTableSectionElement> t_foot();
    WebIDL::ExceptionOr<void> set_t_foot(HTMLTableSectionElement* tfoot);
    GC::Ref<HTMLTableSectionElement> create_t_foot();
    void delete_t_foot();

    GC::Ref<DOM::HTMLCollection> t_bodies();
    GC::Ref<HTMLTableSectionElement> create_t_body();

    GC::Ref<DOM::HTMLCollection> rows();
    WebIDL::ExceptionOr<GC::Ref<HTMLTableRowElement>> insert_row(WebIDL::Long index);
    WebIDL::ExceptionOr<void> delete_row(WebIDL::Long index);

    // https://www.w3.org/TR/html-aria/#el-table
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::table; }

    unsigned border() const;
    [[nodiscard]] Optional<u32> cellpadding() const;

private:
    HTMLTableElement(DOM::Document&, DOM::QualifiedName);

    virtual bool is_html_table_element() const override { return true; }

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual bool is_presentational_hint(FlyString const&) const override;
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const override;
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    GC::Ptr<DOM::HTMLCollection> mutable m_rows;
    GC::Ptr<DOM::HTMLCollection> mutable m_t_bodies;
    Optional<u32> m_cellpadding;
};

}

namespace Web::DOM {
template<>
inline bool Node::fast_is<HTML::HTMLTableElement>() const { return is_html_table_element(); }
}
