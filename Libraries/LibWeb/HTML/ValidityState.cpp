/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ValidityStatePrototype.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/ValidityState.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(ValidityState);

GC::Ref<ValidityState> ValidityState::create(JS::Realm& realm, FormAssociatedElement const& control)
{
    return realm.create<ValidityState>(realm, control);
}

ValidityState::ValidityState(JS::Realm& realm, FormAssociatedElement const& control)
    : PlatformObject(realm)
    , m_control(control)
{
}

void ValidityState::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ValidityState);
}

void ValidityState::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_control.form_associated_element_to_html_element());
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-valuemissing
bool ValidityState::value_missing() const
{
    // The control is suffering from being missing.
    return m_control.suffering_from_being_missing();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-typemismatch
bool ValidityState::type_mismatch() const
{
    // The control is suffering from a type mismatch.
    return m_control.suffering_from_a_type_mismatch();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-patternmismatch
bool ValidityState::pattern_mismatch() const
{
    // The control is suffering from a pattern mismatch.
    return m_control.suffering_from_a_pattern_mismatch();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-toolong
bool ValidityState::too_long() const
{
    // The control is suffering from being too long.
    return m_control.suffering_from_being_too_long();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-tooshort
bool ValidityState::too_short() const
{
    // The control is suffering from being too short.
    return m_control.suffering_from_being_too_short();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-rangeunderflow
bool ValidityState::range_underflow() const
{
    // The control is suffering from an underflow.
    return m_control.suffering_from_an_underflow();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-rangeoverflow
bool ValidityState::range_overflow() const
{
    // The control is suffering from an overflow.
    return m_control.suffering_from_an_overflow();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-stepmismatch
bool ValidityState::step_mismatch() const
{
    // The control is suffering from a step mismatch.
    return m_control.suffering_from_a_step_mismatch();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-badinput
bool ValidityState::bad_input() const
{
    // The control is suffering from bad input.
    return m_control.suffering_from_bad_input();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-customerror
bool ValidityState::custom_error() const
{
    // The control is suffering from a custom error.
    return m_control.suffering_from_a_custom_error();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-valid
bool ValidityState::valid() const
{
    return !(value_missing() || type_mismatch() || pattern_mismatch() || too_long() || too_short() || range_underflow() || range_overflow() || step_mismatch() || bad_input() || custom_error());
}

}
