/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PrototypeObject.h>
#include <LibJS/Runtime/ShadowRealm.h>

namespace JS {

class ShadowRealmPrototype final : public PrototypeObject<ShadowRealmPrototype, ShadowRealm> {
    JS_PROTOTYPE_OBJECT(ShadowRealmPrototype, ShadowRealm, ShadowRealm);
    JS_DECLARE_ALLOCATOR(ShadowRealmPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~ShadowRealmPrototype() override = default;

private:
    explicit ShadowRealmPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(evaluate);
    JS_DECLARE_NATIVE_FUNCTION(import_value);
};

}
