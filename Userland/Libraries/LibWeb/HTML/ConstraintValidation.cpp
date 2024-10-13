/*
 * Copyright (c) 2024, Benjamin Bjerken <beuss-git@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/ConstraintValidation.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLDataListElement.h>
#include <LibWeb/HTML/ValidityState.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-validity
JS::NonnullGCPtr<ValidityState const> ConstraintValidation::validity(DOM::Element const& element) const
{
    if (!m_validity) {
        auto& vm = element.vm();
        auto& realm = element.realm();
        m_validity = vm.heap().allocate<ValidityState>(realm, realm, *this);
    }

    return *m_validity;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-willvalidate
bool ConstraintValidation::will_validate(DOM::Element const& element) const
{
    dbgln("(STUBBED) ConstraintValidation::will_validate(). Called on: {}", element.debug_description());
    return false;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-setcustomvalidity
void ConstraintValidation::set_custom_validity(String const& error, DOM::Element const& element)
{
    (void)error;
    dbgln("(STUBBED) ConstraintValidation::set_custom_validity(). Called on: {}", element.debug_description());
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-checkvalidity
WebIDL::ExceptionOr<bool> ConstraintValidation::check_validity(DOM::Element const& element)
{
    dbgln("(STUBBED) ConstraintValidation::check_validity(). Called on: {}", element.debug_description());
    return true;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-reportvalidity
WebIDL::ExceptionOr<bool> ConstraintValidation::report_validity(DOM::Element const& element)
{
    dbgln("(STUBBED) ConstraintValidation::report_validity(). Called on: {}", element.debug_description());
    return true;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-validationmessage
String ConstraintValidation::validation_message(DOM::Element const& element) const
{
    dbgln("(STUBBED) ConstraintValidation::validation_message(). Called on: {}", element.debug_description());
    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#candidate-for-constraint-validation
bool ConstraintValidation::is_candidate_for_constraint_validation(DOM::Element const& element) const
{
    VERIFY(is<FormAssociatedElement>(element));

    auto const& form_associated_element = dynamic_cast<FormAssociatedElement const&>(element);

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#definitions
    // A submittable element is a candidate for constraint validation...
    if (!form_associated_element.is_submittable()) {
        return false;
    }

    // NOTE: These two checks are valid for all (form associated) elements,
    // so we write them here instead of in the specific implementation of is_barred_from_constraint_validation() for each element.

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#enabling-and-disabling-form-controls:-the-disabled-attribute
    if (element.is_actually_disabled()) {
        return false;
    }

    // https://html.spec.whatwg.org/multipage/form-elements.html#the-datalist-element:barred-from-constraint-validation
    if (element.first_ancestor_of_type<HTML::HTMLDataListElement>()) {
        return false;
    }

    return !is_barred_from_constraint_validation();
}
}
