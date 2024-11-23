/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/BigFraction/BigFraction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/ISORecords.h>

namespace JS::Temporal {

class PlainDateTime final : public Object {
    JS_OBJECT(PlainDateTime, Object);
    GC_DECLARE_ALLOCATOR(PlainDateTime);

public:
    virtual ~PlainDateTime() override = default;

    [[nodiscard]] ISODateTime iso_date_time() const { return m_iso_date_time; }
    [[nodiscard]] String const& calendar() const { return m_calendar; }

private:
    PlainDateTime(ISODateTime const&, String calendar, Object& prototype);

    ISODateTime m_iso_date_time; // [[ISODateTime]]
    String m_calendar;           // [[Calendar]]
};

ISODateTime combine_iso_date_and_time_record(ISODate, Time);
bool iso_date_time_within_limits(ISODateTime);
ThrowCompletionOr<ISODateTime> interpret_temporal_date_time_fields(VM&, StringView calendar, CalendarFields&, Overflow);
ThrowCompletionOr<GC::Ref<PlainDateTime>> to_temporal_date_time(VM&, Value item, Value options = js_undefined());
ISODateTime balance_iso_date_time(double year, double month, double day, double hour, double minute, double second, double millisecond, double microsecond, double nanosecond);
ThrowCompletionOr<GC::Ref<PlainDateTime>> create_temporal_date_time(VM&, ISODateTime const&, String calendar, GC::Ptr<FunctionObject> new_target = {});
i8 compare_iso_date_time(ISODateTime const&, ISODateTime const&);
ThrowCompletionOr<InternalDuration> difference_iso_date_time(VM&, ISODateTime const&, ISODateTime const&, StringView calendar, Unit largest_unit);
ThrowCompletionOr<InternalDuration> difference_plain_date_time_with_rounding(VM&, ISODateTime const&, ISODateTime const&, StringView calendar, Unit largest_unit, u64 rounding_increment, Unit smallest_unit, RoundingMode);
ThrowCompletionOr<Crypto::BigFraction> difference_plain_date_time_with_total(VM&, ISODateTime const&, ISODateTime const&, StringView calendar, Unit);

}
