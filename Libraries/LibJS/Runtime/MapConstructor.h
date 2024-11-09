/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class MapConstructor final : public NativeFunction {
    JS_OBJECT(MapConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(MapConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~MapConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject&) override;

private:
    explicit MapConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(group_by);

    JS_DECLARE_NATIVE_FUNCTION(symbol_species_getter);
};

}
