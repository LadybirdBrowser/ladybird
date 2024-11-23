/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/ISORecords.h>

namespace JS::Temporal {

class PlainTime final : public Object {
    JS_OBJECT(PlainTime, Object);
    GC_DECLARE_ALLOCATOR(PlainTime);

public:
    virtual ~PlainTime() override = default;

    [[nodiscard]] Time const& time() const { return m_time; }

private:
    PlainTime(Time const&, Object& prototype);

    Time m_time; // [[Time]]
};

// Table 5: TemporalTimeLike Record Fields, https://tc39.es/proposal-temporal/#table-temporal-temporaltimelike-record-fields
struct TemporalTimeLike {
    static TemporalTimeLike zero()
    {
        return { .hour = 0, .minute = 0, .second = 0, .millisecond = 0, .microsecond = 0, .nanosecond = 0 };
    }

    Optional<double> hour;
    Optional<double> minute;
    Optional<double> second;
    Optional<double> millisecond;
    Optional<double> microsecond;
    Optional<double> nanosecond;
};

enum class Completeness {
    Complete,
    Partial,
};

Time create_time_record(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond, double delta_days = 0);
Time midnight_time_record();
Time noon_time_record();
TimeDuration difference_time(Time const&, Time const&);
ThrowCompletionOr<GC::Ref<PlainTime>> to_temporal_time(VM&, Value item, Value options = js_undefined());
ThrowCompletionOr<Time> regulate_time(VM&, double hour, double minute, double second, double millisecond, double microsecond, double nanosecond, Overflow);
bool is_valid_time(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond);
Time balance_time(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond);
Time balance_time(double hour, double minute, double second, double millisecond, double microsecond, TimeDuration const& nanosecond);
ThrowCompletionOr<GC::Ref<PlainTime>> create_temporal_time(VM&, Time const&, GC::Ptr<FunctionObject> new_target = {});
ThrowCompletionOr<TemporalTimeLike> to_temporal_time_record(VM&, Object const& temporal_time_like, Completeness = Completeness::Complete);
String time_record_to_string(Time const&, SecondsStringPrecision::Precision);
i8 compare_time_record(Time const&, Time const&);
Time add_time(Time const&, TimeDuration const& time_duration);
Time round_time(Time const&, u64 increment, Unit, RoundingMode);

}
