/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PrototypeObject.h>
#include <LibJS/Runtime/Temporal/Duration.h>

namespace JS::Temporal {

class DurationPrototype final : public PrototypeObject<DurationPrototype, Duration> {
    JS_PROTOTYPE_OBJECT(DurationPrototype, Duration, Temporal.Duration);
    GC_DECLARE_ALLOCATOR(DurationPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~DurationPrototype() override = default;

private:
    explicit DurationPrototype(Realm&);

#define __JS_ENUMERATE(unit) \
    JS_DECLARE_NATIVE_FUNCTION(unit##_getter);
    JS_ENUMERATE_DURATION_UNITS
#undef __JS_ENUMERATE
};

}
