/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLOrSVGOrMathMLElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/MathML/MathMLElement.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dom.html#dom-dataset-dev
template<typename ElementBase>
GC::Ref<DOMStringMap> HTMLOrSVGOrMathMLElement<ElementBase>::dataset()
{
    if (!m_dataset)
        m_dataset = DOMStringMap::create(*static_cast<ElementBase*>(this));
    return *m_dataset;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-focus
template<typename ElementBase>
void HTMLOrSVGOrMathMLElement<ElementBase>::focus()
{
    // 1. If the allow focus steps given this's node document return false, then return.
    if (!static_cast<ElementBase*>(this)->document().allow_focus())
        return;

    // 2. Run the focusing steps for this.
    run_focusing_steps(static_cast<ElementBase*>(this), nullptr, FocusTrigger::Script);

    // FIXME: 3. If options["focusVisible"] is true, or does not exist but in an implementation-defined way the user agent determines it would be best to do so, then indicate focus.

    // FIXME: 4. If options["preventScroll"] is false, then scroll a target into view given this, "auto", "center", and "center".
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-blur
template<typename ElementBase>
void HTMLOrSVGOrMathMLElement<ElementBase>::blur()
{
    // 1. The user agent should run the unfocusing steps given this.
    //    User agents may instead selectively or uniformly do nothing, for usability reasons.
    run_unfocusing_steps(static_cast<ElementBase*>(this));
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#dom-noncedelement-nonce
template<typename ElementBase>
void HTMLOrSVGOrMathMLElement<ElementBase>::attribute_changed(FlyString const& local_name, Optional<String> const&, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    // 1. If element does not include HTMLOrSVGOrMathMLElement, then return.
    // 2. If localName is not nonce or namespace is not null, then return.
    if (local_name != HTML::AttributeNames::nonce || namespace_.has_value())
        return;

    // 3. If value is null, then set element's [[CryptographicNonce]] to the empty string.
    if (!value.has_value()) {
        m_cryptographic_nonce = {};
    }

    // 4. Otherwise, set element's [[CryptographicNonce]] to value.
    else {
        m_cryptographic_nonce = value.value();
    }
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#dom-noncedelement-nonce
template<typename ElementBase>
WebIDL::ExceptionOr<void> HTMLOrSVGOrMathMLElement<ElementBase>::cloned(DOM::Node& copy, bool) const
{
    // The cloning steps for elements that include HTMLOrSVGOrMathMLElement given node, copy, and subtree
    // are to set copy's [[CryptographicNonce]] to node's [[CryptographicNonce]].
    static_cast<ElementBase&>(copy).m_cryptographic_nonce = m_cryptographic_nonce;
    return {};
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#dom-noncedelement-nonce
template<typename ElementBase>
void HTMLOrSVGOrMathMLElement<ElementBase>::inserted()
{
    // Whenever an element including HTMLOrSVGOrMathMLElement becomes browsing-context connected, the user
    // agent must execute the following steps on the element:
    DOM::Element& element = *static_cast<ElementBase*>(this);

    // "A node becomes browsing-context connected when the insertion steps are invoked with it as the argument
    // and it is now browsing-context connected."
    // https://html.spec.whatwg.org/multipage/infrastructure.html#becomes-browsing-context-connected
    if (!element.shadow_including_root().is_browsing_context_connected())
        return;

    // 1. Let CSP list be element's shadow-including root's policy container's CSP list.
    auto csp_list = element.shadow_including_root().document().policy_container()->csp_list;

    // 2. If CSP list contains a header-delivered Content Security Policy, and element has a
    //    nonce content attribute whose value is not the empty string, then:
    if (csp_list->contains_header_delivered_policy() && element.has_attribute(HTML::AttributeNames::nonce)) {
        // 2.1. Let nonce be element's [[CryptographicNonce]].
        auto nonce = m_cryptographic_nonce;

        // 2.2. Set an attribute value for element using "nonce" and the empty string.
        element.set_attribute_value(HTML::AttributeNames::nonce, {});

        // 2.3. Set element's [[CryptographicNonce]] to nonce.
        m_cryptographic_nonce = nonce;
    }

    // https://html.spec.whatwg.org/multipage/interaction.html#the-autofocus-attribute
    if (element.has_attribute(HTML::AttributeNames::autofocus)) {
        // When an element with the autofocus attribute specified is inserted into a document, run the following steps:

        // FIXME: 1. If the user has indicated (for example, by starting to type in a form control) that they do not
        //           wish focus to be changed, then optionally return.

        // 2. Let target be the element's node document.
        auto& target = element.document();

        // 3. If target is not fully active, then return.
        if (!target.is_fully_active())
            return;

        // 4. If target's active sandboxing flag set has the sandboxed automatic features browsing
        //    context flag, then return.
        if (has_flag(target.active_sandboxing_flag_set(), HTML::SandboxingFlagSet::SandboxedAutomaticFeatures))
            return;

        // 5. If the allow focus steps given target return false, then return.
        if (!target.allow_focus())
            return;

        // 6. Let topDocument be target's node navigable's top-level traversable's active document.
        auto top_document = target.navigable()->top_level_traversable()->active_document();

        // 7. If topDocument's autofocus processed flag is false, then remove the element from topDocument's autofocus
        //    candidates, and append the element to topDocument's autofocus candidates.
        if (!top_document->autofocus_processed_flag()) {
            auto& candidates = top_document->autofocus_candidates();
            candidates.remove_first_matching([&element](auto const& other) { return other.ptr() == &element; });
            candidates.append(GC::Ref { element });
        }
    }
}

template<typename ElementBase>
void HTMLOrSVGOrMathMLElement<ElementBase>::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_dataset);
}

template class HTMLOrSVGOrMathMLElement<HTMLElement>;
template class HTMLOrSVGOrMathMLElement<MathML::MathMLElement>;
template class HTMLOrSVGOrMathMLElement<SVG::SVGElement>;

}
