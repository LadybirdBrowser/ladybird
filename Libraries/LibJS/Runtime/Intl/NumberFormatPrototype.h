/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Intl/NumberFormat.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS::Intl {

class NumberFormatPrototype final : public PrototypeObject<NumberFormatPrototype, NumberFormat> {
    JS_PROTOTYPE_OBJECT(NumberFormatPrototype, NumberFormat, Intl.NumberFormat);
    JS_DECLARE_ALLOCATOR(NumberFormatPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~NumberFormatPrototype() override = default;

private:
    explicit NumberFormatPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(format);
    JS_DECLARE_NATIVE_FUNCTION(format_to_parts);
    JS_DECLARE_NATIVE_FUNCTION(format_range);
    JS_DECLARE_NATIVE_FUNCTION(format_range_to_parts);
    JS_DECLARE_NATIVE_FUNCTION(resolved_options);
};

}
