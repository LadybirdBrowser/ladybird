/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLOrSVGElement.h>
#include <LibWeb/MathML/MathMLElement.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dom.html#dom-dataset-dev
template<typename ElementBase>
GC::Ref<DOMStringMap> HTMLOrSVGElement<ElementBase>::dataset()
{
    if (!m_dataset)
        m_dataset = DOMStringMap::create(*static_cast<ElementBase*>(this));
    return *m_dataset;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-focus
template<typename ElementBase>
void HTMLOrSVGElement<ElementBase>::focus()
{
    // 1. If the allow focus steps given the element's node document return false, then return.
    if (!static_cast<ElementBase*>(this)->document().allow_focus())
        return;

    // 2. If the element is marked as locked for focus, then return.
    if (m_locked_for_focus)
        return;

    // 3. Mark the element as locked for focus.
    m_locked_for_focus = true;

    // 4. Run the focusing steps for the element.
    run_focusing_steps(static_cast<ElementBase*>(this));

    // FIXME: 5. If the value of the focusVisible dictionary member of options is true, or is not present but in an implementation-defined way the user agent determines it would be best to do so, then indicate focus.

    // FIXME: 6. If the value of the preventScroll dictionary member of options is false,
    //           then scroll the element into view with scroll behavior "auto",
    //           block flow direction position set to an implementation-defined value,
    //           and inline base direction position set to an implementation-defined value.

    // 7. Unmark the element as locked for focus.
    m_locked_for_focus = false;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-blur
template<typename ElementBase>
void HTMLOrSVGElement<ElementBase>::blur()
{
    // The blur() method, when invoked, should run the unfocusing steps for the element on which the method was called.
    run_unfocusing_steps(static_cast<ElementBase*>(this));

    // User agents may selectively or uniformly ignore calls to this method for usability reasons.
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#dom-noncedelement-nonce
template<typename ElementBase>
void HTMLOrSVGElement<ElementBase>::attribute_changed(FlyString const& local_name, Optional<String> const&, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    // 1. If element does not include HTMLOrSVGElement, then return.
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
WebIDL::ExceptionOr<void> HTMLOrSVGElement<ElementBase>::cloned(DOM::Node& copy, bool) const
{
    // The cloning steps for elements that include HTMLOrSVGElement given node, copy, and subtree
    // are to set copy's [[CryptographicNonce]] to node's [[CryptographicNonce]].
    static_cast<ElementBase&>(copy).m_cryptographic_nonce = m_cryptographic_nonce;
    return {};
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#dom-noncedelement-nonce
template<typename ElementBase>
void HTMLOrSVGElement<ElementBase>::inserted()
{
    // Whenever an element including HTMLOrSVGElement becomes browsing-context connected, the user
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
}

template<typename ElementBase>
void HTMLOrSVGElement<ElementBase>::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_dataset);
}

template class HTMLOrSVGElement<HTMLElement>;
template class HTMLOrSVGElement<MathML::MathMLElement>;
template class HTMLOrSVGElement<SVG::SVGElement>;

}
