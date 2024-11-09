/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/FinalizationRegistry.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS {

class FinalizationRegistryPrototype final : public PrototypeObject<FinalizationRegistryPrototype, FinalizationRegistry> {
    JS_PROTOTYPE_OBJECT(FinalizationRegistryPrototype, FinalizationRegistry, FinalizationRegistry);
    JS_DECLARE_ALLOCATOR(FinalizationRegistryPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~FinalizationRegistryPrototype() override = default;

private:
    explicit FinalizationRegistryPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(cleanup_some);
    JS_DECLARE_NATIVE_FUNCTION(register_);
    JS_DECLARE_NATIVE_FUNCTION(unregister);
};

}
