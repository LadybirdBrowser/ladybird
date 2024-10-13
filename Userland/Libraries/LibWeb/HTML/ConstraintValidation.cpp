/*
 * Copyright (c) 2024, Benjamin Bjerken <beuss-git@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/ConstraintValidation.h>
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
    dbgln("(STUBBED) ConstraintValidation::is_candidate_for_constraint_validation(). Called on: {}", element.debug_description());
    return true;
}
}
