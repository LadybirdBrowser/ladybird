/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Benjamin Bjerken <beuss-git@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ValidityStatePrototype.h>
#include <LibWeb/HTML/ConstraintValidation.h>
#include <LibWeb/HTML/ValidityState.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(ValidityState);

ValidityState::ValidityState(JS::Realm& realm, ConstraintValidation const& associated_element)
    : PlatformObject(realm)
    , m_associated_element(associated_element)
{
}

void ValidityState::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ValidityState);
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#suffering-from-being-missing
bool ValidityState::value_missing() const
{
    return m_associated_element.is_value_missing();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#suffering-from-a-type-mismatch
bool ValidityState::type_mismatch() const
{
    return m_associated_element.is_type_mismatch();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#suffering-from-a-pattern-mismatch
bool ValidityState::pattern_mismatch() const
{
    return m_associated_element.is_pattern_mismatch();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#suffering-from-being-too-long
bool ValidityState::too_long() const
{
    return m_associated_element.is_too_long();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#suffering-from-being-too-short
bool ValidityState::too_short() const
{
    return m_associated_element.is_too_short();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#suffering-from-an-underflow
bool ValidityState::range_underflow() const
{
    return m_associated_element.is_range_underflow();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#suffering-from-an-overflow
bool ValidityState::range_overflow() const
{
    return m_associated_element.is_range_overflow();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#suffering-from-a-step-mismatch
bool ValidityState::step_mismatch() const
{
    return m_associated_element.is_step_mismatch();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#suffering-from-bad-input
bool ValidityState::bad_input() const
{
    return m_associated_element.is_bad_input();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#suffering-from-a-custom-error
bool ValidityState::custom_error() const
{
    return m_associated_element.has_custom_error();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-valid
bool ValidityState::valid() const
{
    return m_associated_element.is_valid();
}

}
