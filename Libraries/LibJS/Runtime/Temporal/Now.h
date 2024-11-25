/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Object.h>

namespace JS::Temporal {

class Now final : public Object {
    JS_OBJECT(Now, Object);
    GC_DECLARE_ALLOCATOR(Now);

public:
    virtual void initialize(Realm&) override;
    virtual ~Now() override = default;

private:
    explicit Now(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(time_zone_id);
    JS_DECLARE_NATIVE_FUNCTION(instant);
    JS_DECLARE_NATIVE_FUNCTION(plain_date_time_iso);
    JS_DECLARE_NATIVE_FUNCTION(zoned_date_time_iso);
    JS_DECLARE_NATIVE_FUNCTION(plain_date_iso);
    JS_DECLARE_NATIVE_FUNCTION(plain_time_iso);
};

Crypto::SignedBigInteger system_utc_epoch_nanoseconds(VM&);
ThrowCompletionOr<ISODateTime> system_date_time(VM&, Value temporal_time_zone_like);

}
