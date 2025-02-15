/*
 * Copyright (c) 2023, Preston Taylor <95388976+PrestonLTaylor@users.noreply.github.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGURIReference.h>

namespace Web::SVG {

class SVGUseElement final
    : public SVGGraphicsElement
    , public SVGURIReferenceMixin<SupportsXLinkHref::Yes> {
    WEB_PLATFORM_OBJECT(SVGUseElement, SVGGraphicsElement);
    GC_DECLARE_ALLOCATOR(SVGUseElement);

public:
    virtual ~SVGUseElement() override = default;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    virtual void inserted() override;

    void svg_element_changed(SVGElement&);
    void svg_element_removed(SVGElement&);

    GC::Ref<SVGAnimatedLength> x() const;
    GC::Ref<SVGAnimatedLength> y() const;
    GC::Ref<SVGAnimatedLength> width() const;
    GC::Ref<SVGAnimatedLength> height() const;

    GC::Ptr<SVGElement> instance_root() const;
    GC::Ptr<SVGElement> animated_instance_root() const;

    virtual Gfx::AffineTransform element_transform() const override;

private:
    SVGUseElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual bool is_svg_use_element() const override { return true; }

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

    void process_the_url(Optional<String> const& href);

    static Optional<FlyString> parse_id_from_href(StringView);

    GC::Ptr<DOM::Element> referenced_element();

    void fetch_the_document(URL::URL const& url);
    bool is_referrenced_element_same_document() const;

    void clone_element_tree_as_our_shadow_tree(Element* to_clone);
    bool is_valid_reference_element(Element const& reference_element) const;

    Optional<float> m_x;
    Optional<float> m_y;

    Optional<URL::URL> m_href;

    GC::Ptr<DOM::DocumentObserver> m_document_observer;
    GC::Ptr<HTML::SharedResourceRequest> m_resource_request;
    Optional<DOM::DocumentLoadEventDelayer> m_load_event_delayer;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGUseElement>() const { return is_svg_use_element(); }

}
