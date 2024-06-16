/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
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

private:
    ValidityState(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
