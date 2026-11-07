/*
 * Copyright (c) 2023, Preston Taylor <95388976+PrestonLTaylor@users.noreply.github.com>
 * Copyright (c) 2024-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGC/HeapHashTable.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/SharedResourceRequest.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/FragmentIdentifier.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/SVG/SVGSymbolElement.h>
#include <LibWeb/SVG/SVGUseElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGUseElement);

SVGUseElement::SVGUseElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

void SVGUseElement::initialize_element()
{
    // NOTE: The spec says "The shadow tree is open (inspectable by script), but read-only."
    //       This doesn't actually match other browsers, and there's a spec issue to change it.
    //       Spec bug: https://github.com/w3c/svgwg/issues/875
    auto shadow_root = DOM::ShadowRoot::create(document(), *this, Web::DOM::ShadowRootMode::Closed);
    shadow_root->set_user_agent_internal(true);

    // The user agent must create a use-element shadow tree whose host is the ‘use’ element itself
    set_shadow_root(shadow_root);

    m_document_observer = DOM::DocumentObserver::create(document());
    m_document_observer->set_document_readiness_observer(GC::create_function(GC::Heap::the(), GC::captures(*this), [](SVGUseElement& self, HTML::DocumentReadyState readiness) {
        if (readiness != HTML::DocumentReadyState::Interactive)
            return;
        // The href processing path already populated the shadow tree for resolved references,
        // unless the referenced subtree changed while the document was still being parsed.
        if (self.instance_root() && !self.m_needs_parsing_complete_reclone)
            return;
        self.m_needs_parsing_complete_reclone = false;
        self.clone_element_tree_as_our_shadow_tree(self.referenced_element());
    }));
}

void SVGUseElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
    visitor.visit(m_document_observer);
    visitor.visit(m_resource_request);
}

void SVGUseElement::adopted_from(DOM::Document& old_document)
{
    Base::adopted_from(old_document);

    m_document_observer->retarget_for_adoption(document());

    // Adoption removed us from our tree, so nothing of ours renders, and our shadow tree is a clone made for the old
    // document; our href may resolve to something else in this one. Drop it, and leave the new clone to inserted(),
    // once we're in a document tree again. A clone made here would join the tree being adopted — so a nested use
    // element in it would then run the adopting steps as if it had come from the old document.
    discard_the_url();
}

void SVGUseElement::inserted()
{
    Base::inserted();

    // Only use elements in the document's node tree react to changes of the elements they reference, mirroring the
    // document-wide traversal this registry replaced.
    if (root().is_document())
        register_for_referenced_element_changes();

    // The insertion that connected us may have inserted our referenced element along with us, with its insertion
    // steps running before ours, i.e. before we were registered to be notified about changes to it. Adoption also
    // drops our shadow tree (see adopted_from()), we may now be in an author's shadow tree, and an external document
    // may have finished loading while we were disconnected. If our shadow tree has not been populated yet, or was
    // copied from an element our reference no longer resolves to, try resolving the reference again.
    if (!is_connected())
        return;
    if (auto instance_root = this->instance_root()) {
        if (instance_root->corresponding_element().ptr() != referenced_element().ptr())
            request_reclone();
        return;
    }

    if (!m_href.has_value()) {
        process_the_url(href_value());
        return;
    }

    if (auto to_clone = referenced_element())
        clone_element_tree_as_our_shadow_tree(to_clone);
}

void SVGUseElement::removed_from(IsSubtreeRoot is_subtree_root, Node* old_ancestor, Node& old_root)
{
    Base::removed_from(is_subtree_root, old_ancestor, old_root);

    if (old_root.is_document())
        unregister_for_referenced_element_changes();

    // NB: The request of a disconnected use element must not delay the load event, which matters for an instance in a
    //     shadow tree that is being discarded. inserted() processes the URL again.
    if (!is_connected() && m_load_event_delayer.has_value()) {
        stop_using_resource_request();
        m_href = {};
    }
}

void SVGUseElement::moved_from(IsSubtreeRoot is_subtree_root, GC::Ptr<Node> old_ancestor)
{
    Base::moved_from(is_subtree_root, old_ancestor);

    if (!old_ancestor)
        return;

    auto was_in_document_tree = old_ancestor->root().is_document();
    auto is_in_document_tree = root().is_document();
    if (was_in_document_tree == is_in_document_tree)
        return;

    if (was_in_document_tree) {
        unregister_for_referenced_element_changes();
        return;
    }

    register_for_referenced_element_changes();

    if (!instance_root()
        && m_href.has_value() && m_href->fragment().has_value() && is_referenced_element_same_document()) {
        clone_element_tree_as_our_shadow_tree(referenced_element());
    }
}

void SVGUseElement::finalize()
{
    Base::finalize();

    // A GC'ed "use" element may never run its removal steps. So, unlink it from the document's list of use elements
    // here — to avoid destroying a still-linked list node.
    unregister_for_referenced_element_changes();
}

void SVGUseElement::register_for_referenced_element_changes()
{
    if (m_list_node.is_in_list())
        return;
    document().register_svg_use_element({}, *this);
}

void SVGUseElement::unregister_for_referenced_element_changes()
{
    if (!m_list_node.is_in_list())
        return;
    document().unregister_svg_use_element({}, *this);
}

void SVGUseElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == SVG::AttributeNames::href || name == SVG::AttributeNames::xlink_href) {
        // AD-HOC: A disconnected use element processes the URL once it is inserted. By then, an instance is linked to
        //         its corresponding element, and cycle checks can see its enclosing instances.
        if (!is_connected()) {
            discard_the_url();
            return;
        }
        // When the ‘href’ attribute is set (or, in the absence of an ‘href’ attribute, an ‘xlink:href’ attribute), the user agent must process the URL.
        process_the_url(value);
    }
}

Optional<Utf16String> SVGUseElement::href_value() const
{
    if (auto href = get_attribute_ns(Optional<Utf16FlyString> {}, AttributeNames::href); href.has_value())
        return href;
    return get_attribute_ns(Namespace::XLink, AttributeNames::href);
}

DOM::Document const& SVGUseElement::document_containing_reference() const
{
    if (auto source = corresponding_element())
        return source->document();
    return document();
}

// https://www.w3.org/TR/SVG2/linking.html#processingURL
void SVGUseElement::process_the_url(Optional<Utf16String> const& href)
{
    // NB: A request for a document that is no longer referenced must neither update the shadow tree nor delay the load
    //     event.
    stop_using_resource_request();

    // In all other cases, the URL is for a resource to be used in this SVG document. The user agent
    // must parse the URL to separate out the target fragment from the rest of the URL, and compare
    // it with the document base URL. If all parts other than the target fragment are equal, this is
    // a same-document URL reference, and processing the URL must continue as indicated in Identifying
    // the target element with the current document as the referenced document.
    m_href = document_containing_reference().encoding_parse_url(href.value_or({}));
    if (m_href.has_value() && is_referenced_element_same_document()) {
        clone_element_tree_as_our_shadow_tree(referenced_element());
        return;
    }

    // If either the ‘use’ element or the referenced element is altered in a way that causes the ‘use’ element’s URL
    // reference to become unresolved again, then the entire shadow tree for that use element is discarded.
    clone_element_tree_as_our_shadow_tree(nullptr);

    if (m_href.has_value())
        fetch_the_document(*m_href);
}

bool SVGUseElement::is_referenced_element_same_document() const
{
    return m_href->equals(document_containing_reference().base_url(), URL::ExcludeFragment::Yes);
}

void SVGUseElement::svg_element_changed(SVGElement& svg_element)
{
    if (m_reclone_pending)
        return;

    auto to_clone = referenced_element();
    if (!to_clone) {
        return;
    }

    // NOTE: We need to check the ancestor because attribute_changed of a child doesn't call children_changed on the parent(s)
    if (to_clone == GC::Ref { svg_element } || to_clone->is_ancestor_of(svg_element))
        request_reclone();
}

void SVGUseElement::element_instance_changed(Badge<SVGElement>)
{
    if (is_connected() && document().readiness() == HTML::DocumentReadyState::Loading) {
        m_needs_parsing_complete_reclone = true;
        return;
    }

    if (!m_reclone_pending)
        request_reclone();
}

void SVGUseElement::request_reclone()
{
    m_reclone_pending = true;
    if (!is_connected())
        return;
    document().style_computer().style_engine().note_element_awaiting_preparation_for_style_computation(*this);
    set_needs_layout_tree_update(true, DOM::SetNeedsLayoutTreeUpdateReason::SVGUseElementShadowTreeRebuild);
}

void SVGUseElement::prepare_for_style_computation()
{
    if (m_reclone_pending)
        clone_element_tree_as_our_shadow_tree(referenced_element());
}

void SVGUseElement::svg_element_changed_before_parsing_complete(SVGElement& svg_element)
{
    auto to_clone = referenced_element();
    if (!to_clone)
        return;

    // NOTE: We need to check the ancestor because attribute_changed of a child doesn't call children_changed on the parent(s)
    if (to_clone == GC::Ref { svg_element } || to_clone->is_ancestor_of(svg_element))
        m_needs_parsing_complete_reclone = true;
}

void SVGUseElement::svg_element_removed(SVGElement& svg_element)
{
    if (!m_href.has_value() || !m_href->fragment().has_value() || !is_referenced_element_same_document()) {
        return;
    }

    auto id = decode_fragment_identifier(*m_href->fragment());
    if (svg_element.get_attribute_value("id"_utf16_fly_string) == id) {
        shadow_root()->remove_all_children();
    }
}

// https://svgwg.org/svg2-draft/linking.html#processingURL-target
GC::Ptr<DOM::Element> SVGUseElement::referenced_element() const
{
    if (!m_href.has_value())
        return nullptr;

    if (!m_href->fragment().has_value())
        return nullptr;

    if (is_referenced_element_same_document()) {
        auto id = decode_fragment_identifier(*m_href->fragment());
        return document_containing_reference().get_element_by_id(id);
    }

    if (!m_resource_request)
        return nullptr;

    auto data = m_resource_request->image_data();
    if (!data || !is<SVG::SVGDecodedImageData>(*data))
        return nullptr;

    return as<SVG::SVGDecodedImageData>(*data).svg_document().get_element_by_id(decode_fragment_identifier(*m_href->fragment()));
}

void SVGUseElement::discard_the_url()
{
    clone_element_tree_as_our_shadow_tree(nullptr);
    stop_using_resource_request();
    m_href = {};
}

void SVGUseElement::stop_using_resource_request()
{
    if (m_resource_request && m_resource_request_callbacks_id.has_value())
        m_resource_request->remove_callbacks(*m_resource_request_callbacks_id);
    m_resource_request_callbacks_id = {};
    m_resource_request = nullptr;
    m_load_event_delayer.clear();
}

// https://svgwg.org/svg2-draft/linking.html#processingURL-fetch
void SVGUseElement::fetch_the_document(URL::URL const& url)
{
    // AD-HOC: An element in a temporary document for fragment parsing does not fetch anything, so that markup parsed
    //         into such a document, for example by the Sanitizer API, causes no network requests for elements that
    //         are never inserted anywhere. The document is fetched once the element is inserted into another document.
    if (document().is_temporary_document_for_fragment_parsing())
        return;

    // NB: The fragment only identifies the target element within the fetched document, so every reference to the same
    //     external document shares one fetch and one parsed document. The spec allows this re-use under "Processing
    //     the subresource document".
    auto document_url = url;
    document_url.set_fragment({});

    stop_using_resource_request();

    // https://svgwg.org/svg2-draft/struct.html#UseElement
    // User agents may restrict external resource documents for security reasons. In particular, this specification
    // does not allow cross-origin and data URL resource requests in ‘use’.
    if (document_url.scheme() == "data"sv)
        return;

    // For all other references, the "no-cors" state.
    // AD-HOC: Set the same-origin fallback flag, as HTML does for track elements, so that Fetch enforces the ‘use’
    //         origin restriction before sending a request and across redirects.
    auto request = HTML::create_potential_CORS_request(document_url, Fetch::Infrastructure::Request::Destination::Image, HTML::CORSSettingAttribute::NoCORS, HTML::SameOriginFallbackFlag::Yes);
    request->set_client(document().relevant_settings_object().address());

    m_load_event_delayer.emplace(document());
    m_resource_request = HTML::SharedResourceRequest::get_or_create(document(), *request);
    m_resource_request_callbacks_id = m_resource_request->add_removable_callbacks(
        GC::create_function(GC::Heap::the(), GC::captures(*this), [](SVGUseElement& self) {
            // Only a connected use element builds its shadow tree; inserted() builds it otherwise.
            if (self.is_connected())
                self.clone_element_tree_as_our_shadow_tree(self.referenced_element());
            self.m_load_event_delayer.clear();
        }),
        GC::create_function(GC::Heap::the(), GC::captures(*this), [](SVGUseElement& self) {
            self.m_load_event_delayer.clear();
        }),
        GC::create_function(GC::Heap::the(), GC::captures(*this), [](SVGUseElement& self) {
            self.m_load_event_delayer.clear();
        }));

    if (m_resource_request->needs_fetching())
        m_resource_request->fetch_resource(request);
}

// https://svgwg.org/svg2-draft/struct.html#UseShadowTree
void SVGUseElement::clone_element_tree_as_our_shadow_tree(GC::Ptr<Element> to_clone)
{
    m_reclone_pending = false;
    shadow_root()->remove_all_children();

    // https://svgwg.org/svg2-draft/struct.html#UseStyleInheritance
    // When the referenced element is from the same document as the ‘use’ element, the same document stylesheets will
    // apply in both the original document and the shadow tree document fragment.
    shadow_root()->set_uses_document_style_sheets(to_clone && &to_clone->document() == &document());

    if (to_clone && is_valid_reference_element(*to_clone)) {
        // The ‘use’ element references another element, a copy of which is rendered in place of the ‘use’ in the document.
        // Each node in the shadow tree is an instance of a corresponding node from the referenced document subtree.
        // FIXME: If the referenced element is in an external file, then all URL references in attributes and style
        //        properties must be made absolute as described in Generating the absolute URL, before copying the value
        //        to the element instances. The shadow tree itself uses the same document base URL as the document that
        //        includes it.
        auto cloned_reference_node = MUST(to_clone->clone_node(&document(), true));
        link_to_corresponding_elements(as<SVGElement>(*cloned_reference_node), as<SVGElement>(*to_clone));
        if (is<SVGSVGElement>(cloned_reference_node.ptr()) || is<SVGSymbolElement>(cloned_reference_node.ptr())) {
            auto& cloned_element = as<SVGElement>(*cloned_reference_node);

            // The width and height properties on the ‘use’ element override the values for the corresponding
            // properties on a referenced ‘svg’ or ‘symbol’ element when determining the used value for that property
            // on the instance root element. However, if the computed value for the property on the ‘use’ element is
            // auto, then the property is computed as normal for the element instance.
            if (has_attribute(AttributeNames::width)) {
                cloned_element.set_attribute_value(AttributeNames::width, get_attribute_value(AttributeNames::width));
            }
            if (has_attribute(AttributeNames::height)) {
                cloned_element.set_attribute_value(AttributeNames::height, get_attribute_value(AttributeNames::height));
            }
        }
        shadow_root()->append_child(cloned_reference_node).release_value_but_fixme_should_propagate_errors();
    }
}

// https://svgwg.org/svg2-draft/struct.html#TermCorrespondingElement
void SVGUseElement::link_to_corresponding_elements(SVGElement& instance, SVGElement& original)
{
    instance.set_corresponding_element({}, original);

    // NB: Only SVG children are paired up. Inserting a clone's HTML content can change it, for example when an option
    //     element updates a selectedcontent element.
    auto* instance_child = instance.first_child_of_type<SVGElement>();
    for (auto* original_child = original.first_child_of_type<SVGElement>(); original_child && instance_child; original_child = original_child->next_sibling_of_type<SVGElement>()) {
        link_to_corresponding_elements(*instance_child, *original_child);
        instance_child = instance_child->next_sibling_of_type<SVGElement>();
    }
}

bool SVGUseElement::is_valid_reference_element(Element const& reference_element) const
{
    // If the referenced element that results from resolving the URL is not an SVG element, then the reference is invalid and the ‘use’ element is in error.
    if (!reference_element.is_svg_element())
        return false;

    // https://svgwg.org/svg2-draft/struct.html#UseShadowTree
    // When a ‘use’ references another element which is another ‘use’ or whose content contains a ‘use’ element, then
    // the shadow DOM cloning approach described above is recursive. However, a set of references that directly or
    // indirectly reference a element to create a circular dependency is an invalid circular reference. The ‘use’
    // element or element instance whose shadow tree would create the circular reference is in error and must not be
    // rendered by the user agent.
    return !would_create_circular_reference(reference_element);
}

bool SVGUseElement::would_create_circular_reference(Element const& target) const
{
    auto visited = GC::Heap::the().allocate<GC::HeapHashTable<GC::Ref<Element const>>>();
    for (auto* shadow_root = as_if<DOM::ShadowRoot>(root()); shadow_root && shadow_root->host(); shadow_root = as_if<DOM::ShadowRoot>(shadow_root->host()->root())) {
        if (auto const* use = as_if<SVGUseElement>(*shadow_root->host())) {
            if (auto instance = use->instance_root()) {
                if (auto source = instance->corresponding_element())
                    visited->table().set(*source);
            }
        }
    }
    return would_create_circular_reference_impl(target, visited);
}

bool SVGUseElement::would_create_circular_reference_impl(
    Element const& target,
    GC::HeapHashTable<GC::Ref<Element const>>& visited) const
{
    // FIXME: I am certain there is a much more efficient way of keeping track of cycles here!
    if (visited.table().contains(target))
        return true;

    visited.table().set(target);

    bool found_circular_reference = false;
    target.for_each_in_inclusive_subtree_of_type<SVGUseElement>([&](auto& element) {
        auto referenced = element.referenced_element();
        if (!referenced)
            return TraversalDecision::Continue;

        if (would_create_circular_reference_impl(*referenced, visited)) {
            found_circular_reference = true;
            return TraversalDecision::Break;
        }

        return TraversalDecision::Continue;
    });

    visited.table().remove(target);

    return found_circular_reference;
}

// https://svgwg.org/svg2-draft/struct.html#TermInstanceRoot
GC::Ptr<SVGElement> SVGUseElement::instance_root() const
{
    return const_cast<DOM::ShadowRoot&>(*shadow_root()).first_child_of_type<SVGElement>();
}

CSS::ElementBoxKind SVGUseElement::box_kind() const
{
    return CSS::ElementBoxKind::SvgGraphics;
}

}
