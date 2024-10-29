/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLOrSVGElement.h>
#include <LibWeb/MathML/MathMLElement.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dom.html#dom-dataset-dev
template<typename ElementBase>
JS::NonnullGCPtr<DOMStringMap> HTMLOrSVGElement<ElementBase>::dataset()
{
    if (!m_dataset)
        m_dataset = DOMStringMap::create(*static_cast<ElementBase*>(this));
    return *m_dataset;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-focus
template<typename ElementBase>
void HTMLOrSVGElement<ElementBase>::focus()
{
    // FIXME: below are the focus(options) steps, also implement focus()

    // 1. If the element is marked as locked for focus, then return.
    if (m_locked_for_focus)
        return;

    // 2. Mark the element as locked for focus.
    m_locked_for_focus = true;

    // 3. Run the focusing steps for the element.
    run_focusing_steps(static_cast<ElementBase*>(this));

    // FIXME: 4. If the value of the preventScroll dictionary member of options is false,
    //           then scroll the element into view with scroll behavior "auto",
    //           block flow direction position set to an implementation-defined value,
    //           and inline base direction position set to an implementation-defined value.

    // 5. Unmark the element as locked for focus.
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

template<typename ElementBase>
void HTMLOrSVGElement<ElementBase>::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_dataset);
}

template class HTMLOrSVGElement<HTMLElement>;
template class HTMLOrSVGElement<MathML::MathMLElement>;
template class HTMLOrSVGElement<SVG::SVGElement>;

}
