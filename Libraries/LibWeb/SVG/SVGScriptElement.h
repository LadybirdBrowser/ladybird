/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGURIReference.h>

namespace Web::SVG {

// https://www.w3.org/TR/SVG/interact.html#InterfaceSVGScriptElement
class SVGScriptElement
    : public SVGElement
    , public SVGURIReferenceMixin<SupportsXLinkHref::Yes> {
    WEB_PLATFORM_OBJECT(SVGScriptElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGScriptElement);

public:
    void process_the_script_element();

    bool is_parser_inserted() const { return m_parser_inserted; }
    void set_parser_inserted(Badge<HTML::HTMLParser>) { m_parser_inserted = true; }

    bool is_ready_to_be_parser_executed() const { return m_ready_to_be_parser_executed; }
    void execute_pending_parser_blocking_script(Badge<HTML::HTMLParser>);

    void set_source_line_number(Badge<HTML::HTMLParser>, size_t source_line_number) { m_source_line_number = source_line_number; }

    virtual void inserted() override;
    virtual void children_changed(ChildrenChangedMetadata const&) override;
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

protected:
    SVGScriptElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

private:
    virtual bool is_svg_script_element() const final { return true; }

    virtual void visit_edges(Cell::Visitor&) override;

    void finish_external_script_fetch(URL::URL const& script_url, ByteBuffer const& body);
    void execute_script();

    bool m_already_processed { false };
    bool m_parser_inserted { false };
    bool m_ready_to_be_parser_executed { false };

    GC::Ptr<HTML::ClassicScript> m_script;

    Optional<DOM::DocumentLoadEventDelayer> m_document_load_event_delayer;

    size_t m_source_line_number { 1 };
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGScriptElement>() const { return is_svg_script_element(); }

}
