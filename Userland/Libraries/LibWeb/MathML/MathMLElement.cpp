/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/MathMLElementPrototype.h>
#include <LibWeb/MathML/MathMLElement.h>
#include <LibWeb/MathML/TagNames.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLElement);

MathMLElement::~MathMLElement() = default;

MathMLElement::MathMLElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : DOM::Element(document, move(qualified_name))
{
}

void MathMLElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MathMLElement);
}

GC::Ref<HTML::DOMStringMap> MathMLElement::dataset()
{
    if (!m_dataset)
        m_dataset = HTML::DOMStringMap::create(*this);
    return *m_dataset;
}

Optional<ARIA::Role> MathMLElement::default_role() const
{
    // https://www.w3.org/TR/html-aria/#el-math
    if (local_name() == TagNames::math)
        return ARIA::Role::math;
    return {};
}

void MathMLElement::focus()
{
    dbgln("(STUBBED) MathMLElement::focus()");
}

void MathMLElement::blur()
{
    dbgln("(STUBBED) MathMLElement::blur()");
}

void MathMLElement::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_dataset);
}

}
