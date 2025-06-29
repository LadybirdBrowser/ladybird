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

class JS_API PlainTime final : public Object {
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

JS_API Time create_time_record(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond, double delta_days = 0);
JS_API Time midnight_time_record();
JS_API Time noon_time_record();
JS_API TimeDuration difference_time(Time const&, Time const&);
JS_API ThrowCompletionOr<GC::Ref<PlainTime>> to_temporal_time(VM&, Value item, Value options = js_undefined());
JS_API ThrowCompletionOr<Time> to_time_record_or_midnight(VM&, Value item);
JS_API ThrowCompletionOr<Time> regulate_time(VM&, double hour, double minute, double second, double millisecond, double microsecond, double nanosecond, Overflow);
JS_API bool is_valid_time(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond);
JS_API Time balance_time(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond);
JS_API Time balance_time(double hour, double minute, double second, double millisecond, double microsecond, Crypto::SignedBigInteger const& nanosecond);
JS_API ThrowCompletionOr<GC::Ref<PlainTime>> create_temporal_time(VM&, Time const&, GC::Ptr<FunctionObject> new_target = {});
JS_API ThrowCompletionOr<TemporalTimeLike> to_temporal_time_record(VM&, Object const& temporal_time_like, Completeness = Completeness::Complete);
JS_API String time_record_to_string(Time const&, SecondsStringPrecision::Precision);
JS_API i8 compare_time_record(Time const&, Time const&);
JS_API Time add_time(Time const&, TimeDuration const& time_duration);
JS_API Time round_time(Time const&, u64 increment, Unit, RoundingMode);
JS_API ThrowCompletionOr<GC::Ref<Duration>> difference_temporal_plain_time(VM&, DurationOperation, PlainTime const&, Value other, Value options);
JS_API ThrowCompletionOr<GC::Ref<PlainTime>> add_duration_to_time(VM&, ArithmeticOperation, PlainTime const&, Value temporal_duration_like);

}
