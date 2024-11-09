/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Intl/DateTimeFormat.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS::Intl {

class DateTimeFormatPrototype final : public PrototypeObject<DateTimeFormatPrototype, DateTimeFormat> {
    JS_PROTOTYPE_OBJECT(DateTimeFormatPrototype, DateTimeFormat, Intl.DateTimeFormat);
    JS_DECLARE_ALLOCATOR(DateTimeFormatPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~DateTimeFormatPrototype() override = default;

private:
    explicit DateTimeFormatPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(format);
    JS_DECLARE_NATIVE_FUNCTION(format_to_parts);
    JS_DECLARE_NATIVE_FUNCTION(format_range);
    JS_DECLARE_NATIVE_FUNCTION(format_range_to_parts);
    JS_DECLARE_NATIVE_FUNCTION(resolved_options);
};

}
