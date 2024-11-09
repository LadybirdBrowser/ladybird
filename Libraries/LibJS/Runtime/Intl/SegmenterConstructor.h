/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS::Intl {

class SegmenterConstructor final : public NativeFunction {
    JS_OBJECT(SegmenterConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(SegmenterConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~SegmenterConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject& new_target) override;

private:
    explicit SegmenterConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(supported_locales_of);
};

}
