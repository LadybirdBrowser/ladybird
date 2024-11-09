/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PrototypeObject.h>
#include <LibJS/Runtime/WeakSet.h>

namespace JS {

class WeakSetPrototype final : public PrototypeObject<WeakSetPrototype, WeakSet> {
    JS_PROTOTYPE_OBJECT(WeakSetPrototype, WeakSet, WeakSet);
    JS_DECLARE_ALLOCATOR(WeakSetPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~WeakSetPrototype() override = default;

private:
    explicit WeakSetPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(add);
    JS_DECLARE_NATIVE_FUNCTION(delete_);
    JS_DECLARE_NATIVE_FUNCTION(has);
};

}
