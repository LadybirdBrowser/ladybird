/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Intl/DisplayNames.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS::Intl {

class DisplayNamesPrototype final : public PrototypeObject<DisplayNamesPrototype, DisplayNames> {
    JS_PROTOTYPE_OBJECT(DisplayNamesPrototype, DisplayNames, Intl.DisplayNames);
    GC_DECLARE_ALLOCATOR(DisplayNamesPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~DisplayNamesPrototype() override = default;

private:
    explicit DisplayNamesPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(resolved_options);
    JS_DECLARE_NATIVE_FUNCTION(of);
};

}
