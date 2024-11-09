/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Intl/Collator.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS::Intl {

class CollatorPrototype final : public PrototypeObject<CollatorPrototype, Collator> {
    JS_PROTOTYPE_OBJECT(CollatorPrototype, Collator, Collator);
    JS_DECLARE_ALLOCATOR(CollatorPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~CollatorPrototype() override = default;

private:
    explicit CollatorPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(compare_getter);
    JS_DECLARE_NATIVE_FUNCTION(resolved_options);
};

}
