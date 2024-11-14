/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Intl/NumberFormat.h>
#include <LibJS/Runtime/NativeFunction.h>

namespace JS::Intl {

class NumberFormatConstructor final : public NativeFunction {
    JS_OBJECT(NumberFormatConstructor, NativeFunction);
    GC_DECLARE_ALLOCATOR(NumberFormatConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~NumberFormatConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<GC::Ref<Object>> construct(FunctionObject& new_target) override;

private:
    explicit NumberFormatConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(supported_locales_of);
};

ThrowCompletionOr<void> set_number_format_digit_options(VM&, NumberFormatBase& intl_object, Object const& options, int default_min_fraction_digits, int default_max_fraction_digits, Unicode::Notation notation);
ThrowCompletionOr<void> set_number_format_unit_options(VM&, NumberFormat& intl_object, Object const& options);

}
