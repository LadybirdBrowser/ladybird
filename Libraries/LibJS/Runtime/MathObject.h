/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>

namespace JS {

class MathObject final : public Object {
    JS_OBJECT(MathObject, Object);
    GC_DECLARE_ALLOCATOR(MathObject);

public:
    virtual void initialize(Realm&) override;
    virtual ~MathObject() override = default;

    static ThrowCompletionOr<Value> log_impl(VM&, Value);
    static ThrowCompletionOr<Value> sqrt_impl(VM&, Value);
    static ThrowCompletionOr<Value> pow_impl(VM&, Value base, Value exponent);
    static ThrowCompletionOr<Value> floor_impl(VM&, Value);
    static ThrowCompletionOr<Value> ceil_impl(VM&, Value);
    static ThrowCompletionOr<Value> round_impl(VM&, Value);
    static ThrowCompletionOr<Value> exp_impl(VM&, Value);
    static ThrowCompletionOr<Value> abs_impl(VM&, Value);
    static ThrowCompletionOr<Value> sum_precise_impl(VM&, Value);
    static ThrowCompletionOr<Value> imul_impl(VM&, Value, Value);
    static Value random_impl();

private:
    explicit MathObject(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(abs);
    JS_DECLARE_NATIVE_FUNCTION(random);
    JS_DECLARE_NATIVE_FUNCTION(sqrt);
    JS_DECLARE_NATIVE_FUNCTION(floor);
    JS_DECLARE_NATIVE_FUNCTION(ceil);
    JS_DECLARE_NATIVE_FUNCTION(round);
    JS_DECLARE_NATIVE_FUNCTION(max);
    JS_DECLARE_NATIVE_FUNCTION(min);
    JS_DECLARE_NATIVE_FUNCTION(trunc);
    JS_DECLARE_NATIVE_FUNCTION(sin);
    JS_DECLARE_NATIVE_FUNCTION(cos);
    JS_DECLARE_NATIVE_FUNCTION(tan);
    JS_DECLARE_NATIVE_FUNCTION(pow);
    JS_DECLARE_NATIVE_FUNCTION(exp);
    JS_DECLARE_NATIVE_FUNCTION(expm1);
    JS_DECLARE_NATIVE_FUNCTION(sign);
    JS_DECLARE_NATIVE_FUNCTION(clz32);
    JS_DECLARE_NATIVE_FUNCTION(acos);
    JS_DECLARE_NATIVE_FUNCTION(acosh);
    JS_DECLARE_NATIVE_FUNCTION(asin);
    JS_DECLARE_NATIVE_FUNCTION(asinh);
    JS_DECLARE_NATIVE_FUNCTION(atan);
    JS_DECLARE_NATIVE_FUNCTION(atanh);
    JS_DECLARE_NATIVE_FUNCTION(log1p);
    JS_DECLARE_NATIVE_FUNCTION(cbrt);
    JS_DECLARE_NATIVE_FUNCTION(atan2);
    JS_DECLARE_NATIVE_FUNCTION(fround);
    JS_DECLARE_NATIVE_FUNCTION(f16round);
    JS_DECLARE_NATIVE_FUNCTION(hypot);
    JS_DECLARE_NATIVE_FUNCTION(imul);
    JS_DECLARE_NATIVE_FUNCTION(log);
    JS_DECLARE_NATIVE_FUNCTION(log2);
    JS_DECLARE_NATIVE_FUNCTION(log10);
    JS_DECLARE_NATIVE_FUNCTION(sinh);
    JS_DECLARE_NATIVE_FUNCTION(cosh);
    JS_DECLARE_NATIVE_FUNCTION(tanh);
    JS_DECLARE_NATIVE_FUNCTION(sumPrecise);
};

}
