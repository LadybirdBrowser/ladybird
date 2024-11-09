/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Intl/PluralRules.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS::Intl {

class PluralRulesPrototype final : public PrototypeObject<PluralRulesPrototype, PluralRules> {
    JS_PROTOTYPE_OBJECT(PluralRulesPrototype, PluralRules, Intl.PluralRules);
    JS_DECLARE_ALLOCATOR(PluralRulesPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~PluralRulesPrototype() override = default;

private:
    explicit PluralRulesPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(select);
    JS_DECLARE_NATIVE_FUNCTION(select_range);
    JS_DECLARE_NATIVE_FUNCTION(resolved_options);
};

}
