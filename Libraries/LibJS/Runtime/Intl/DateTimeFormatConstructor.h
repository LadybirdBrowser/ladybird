/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS::Intl {

class DateTimeFormatConstructor final : public NativeFunction {
    JS_OBJECT(DateTimeFormatConstructor, NativeFunction);
    GC_DECLARE_ALLOCATOR(DateTimeFormatConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~DateTimeFormatConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<GC::Ref<Object>> construct(FunctionObject& new_target) override;

private:
    explicit DateTimeFormatConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(supported_locales_of);
};

enum class OptionRequired {
    Any,
    Date,
    Time,
    YearMonth,
    MonthDay,
};

enum class OptionDefaults {
    All,
    Date,
    Time,
    YearMonth,
    MonthDay,
    ZonedDateTime,
};

enum class OptionInherit {
    All,
    Relevant,
};

ThrowCompletionOr<GC::Ref<DateTimeFormat>> create_date_time_format(VM&, FunctionObject& new_target, Value locales_value, Value options_value, OptionRequired, OptionDefaults, Optional<String> const& to_locale_string_time_zone = {});
String format_offset_time_zone_identifier(double offset_minutes);

}
