/*
 * Copyright (c) 2023, Preston Taylor <95388976+PrestonLTaylor@users.noreply.github.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntrusiveList.h>
#include <AK/Utf16String.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGURIReference.h>

namespace Web::SVG {

class SVGUseElement final
    : public SVGGraphicsElement
    , public SVGURIReferenceMixin<SupportsXLinkHref::Yes> {
    WEB_WRAPPABLE(SVGUseElement, SVGGraphicsElement);
    GC_DECLARE_ALLOCATOR(SVGUseElement);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~SVGUseElement() override = default;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    void svg_element_changed(SVGElement&, Optional<u64> insertion_generation);
    void svg_element_changed_before_document_complete(SVGElement&, Optional<u64> insertion_generation);
    void svg_element_removed(SVGElement&);

    // AD-HOC: The spec states that the x, y, width and height IDL attributes reflect the respective computed values and their
    //         corresponding presentation attributes but other browsers reflect the attribute values instead - see
    //         https://github.com/w3c/svgwg/issues/1153

    // https://w3c.github.io/svgwg/svg2-draft/struct.html#__svg__SVGUseElement__x
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(x, Horizontal, SVGLengthValue::number(0));

    // https://w3c.github.io/svgwg/svg2-draft/struct.html#__svg__SVGUseElement__y
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(y, Vertical, SVGLengthValue::number(0));

    // https://w3c.github.io/svgwg/svg2-draft/struct.html#__svg__SVGUseElement__width
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(width, Horizontal, SVGLengthValue::number(0));

    // https://w3c.github.io/svgwg/svg2-draft/struct.html#__svg__SVGUseElement__height
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(height, Vertical, SVGLengthValue::number(0));

    GC::Ptr<SVGElement> instance_root() const;

    virtual Gfx::AffineTransform additional_element_transform() const override;

private:
    SVGUseElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize_element() override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;
    virtual void adopted_from(DOM::Document&) override;
    virtual void inserted() override;
    virtual void post_connection() override;
    virtual void removed_from(IsSubtreeRoot, Node* old_ancestor, Node& old_root) override;
    virtual void moved_from(IsSubtreeRoot, GC::Ptr<Node> old_ancestor) override;

    virtual bool is_svg_use_element() const override { return true; }

    virtual Layout::Node* create_layout_node(CSS::LayoutStyle) override;

    void process_the_url(Optional<Utf16String> const& href);
    Optional<Utf16String> href_value() const;

    GC::Ptr<DOM::Element> referenced_element() const;
    bool has_already_applied_insertion_clone(GC::Ptr<DOM::Element>, Optional<u64> insertion_generation) const;

    void fetch_the_document(URL::URL const& url);
    bool is_referenced_element_same_document() const;

    void clone_element_tree_as_our_shadow_tree(GC::Ptr<Element> to_clone);
    bool is_valid_reference_element(Element const& reference_element) const;
    bool would_create_circular_reference(Element const& target) const;
    bool would_create_circular_reference_impl(Element const& target, GC::HeapHashTable<GC::Ref<Element const>>& visited) const;
    void register_for_referenced_element_changes();
    void unregister_for_referenced_element_changes();

    bool m_needs_document_complete_reclone { false };

    Optional<URL::URL> m_href;

    struct InsertionClone {
        u64 generation { 0 };
        GC::Weak<DOM::Element> target;
    };
    InsertionClone m_last_insertion_clone;

    GC::Ptr<DOM::DocumentObserver> m_document_observer;
    GC::Ptr<HTML::SharedResourceRequest> m_resource_request;
    Optional<DOM::DocumentLoadEventDelayer> m_load_event_delayer;

    IntrusiveListNode<SVGUseElement> m_list_node;

public:
    using DocumentUseElementList = IntrusiveList<&SVGUseElement::m_list_node>;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGUseElement>() const { return is_svg_use_element(); }

}
