/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PrototypeObject.h>
#include <LibJS/Runtime/WeakMap.h>

namespace JS {

class WeakMapPrototype final : public PrototypeObject<WeakMapPrototype, WeakMap> {
    JS_PROTOTYPE_OBJECT(WeakMapPrototype, WeakMap, WeakMap);
    JS_DECLARE_ALLOCATOR(WeakMapPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~WeakMapPrototype() override = default;

private:
    explicit WeakMapPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(delete_);
    JS_DECLARE_NATIVE_FUNCTION(get);
    JS_DECLARE_NATIVE_FUNCTION(has);
    JS_DECLARE_NATIVE_FUNCTION(set);
};

}
