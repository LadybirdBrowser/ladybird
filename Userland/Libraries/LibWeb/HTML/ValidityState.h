/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Benjamin Bjerken <beuss-git@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#validitystate
class ValidityState final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(ValidityState, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(ValidityState);

public:
    virtual ~ValidityState() override = default;

    bool value_missing() const;
    bool type_mismatch() const;
    bool pattern_mismatch() const;
    bool too_long() const;
    bool too_short() const;
    bool range_underflow() const;
    bool range_overflow() const;
    bool step_mismatch() const;
    bool bad_input() const;
    bool custom_error() const;
    bool valid() const;

private:
    ValidityState(JS::Realm&, ConstraintValidation const& associated_element);

    virtual void initialize(JS::Realm&) override;

    ConstraintValidation const& m_associated_element;
};

}
