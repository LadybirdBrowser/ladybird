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

void MathMLElement::attribute_changed(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(local_name, old_value, value, namespace_);
    HTMLOrSVGElement::attribute_changed(local_name, old_value, value, namespace_);
}

WebIDL::ExceptionOr<void> MathMLElement::cloned(DOM::Node& node, bool clone_children)
{
    TRY(Base::cloned(node, clone_children));
    TRY(HTMLOrSVGElement::cloned(node, clone_children));
    return {};
}

void MathMLElement::inserted()
{
    Base::inserted();
    HTMLOrSVGElement::inserted();
}

void MathMLElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MathMLElement);
}

Optional<ARIA::Role> MathMLElement::default_role() const
{
    // https://www.w3.org/TR/html-aria/#el-math
    if (local_name() == TagNames::math)
        return ARIA::Role::math;
    return {};
}

void MathMLElement::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    HTMLOrSVGElement::visit_edges(visitor);
}

}
