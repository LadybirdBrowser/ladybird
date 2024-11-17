/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/LazyLoadingElement.h>
#include <LibWeb/HTML/NavigableContainer.h>

namespace Web::HTML {

class HTMLIFrameElement final
    : public NavigableContainer
    , public LazyLoadingElement<HTMLIFrameElement> {

    WEB_PLATFORM_OBJECT(HTMLIFrameElement, NavigableContainer);
    LAZY_LOADING_ELEMENT(HTMLIFrameElement);
    GC_DECLARE_ALLOCATOR(HTMLIFrameElement);

public:
    virtual ~HTMLIFrameElement() override;

    virtual GC::Ptr<Layout::Node> create_layout_node(CSS::StyleProperties) override;
    virtual void adjust_computed_style(CSS::StyleProperties&) override;

    void set_current_navigation_was_lazy_loaded(bool value);

    Optional<HighResolutionTime::DOMHighResTimeStamp> const& pending_resource_start_time() const { return m_pending_resource_start_time; }
    void set_pending_resource_start_time(Optional<HighResolutionTime::DOMHighResTimeStamp> time) { m_pending_resource_start_time = time; }

    GC::Ref<DOM::DOMTokenList> sandbox();

    virtual void visit_edges(Cell::Visitor&) override;

private:
    HTMLIFrameElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    // ^DOM::Element
    virtual void inserted() override;
    virtual void removed_from(Node*) override;
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;
    virtual i32 default_tab_index_value() const override;

    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-iframe-element:dimension-attributes
    virtual bool supports_dimension_attributes() const override { return true; }

    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#process-the-iframe-attributes
    void process_the_iframe_attributes(bool initial_insertion = false);

    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#current-navigation-was-lazy-loaded
    bool m_current_navigation_was_lazy_loaded { false };

    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#iframe-pending-resource-timing-start-time
    Optional<HighResolutionTime::DOMHighResTimeStamp> m_pending_resource_start_time = {};

    GC::Ptr<DOM::DOMTokenList> m_sandbox;
};

void run_iframe_load_event_steps(HTML::HTMLIFrameElement&);

}
