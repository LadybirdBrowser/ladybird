/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Intl/ListFormat.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS::Intl {

class ListFormatPrototype final : public PrototypeObject<ListFormatPrototype, ListFormat> {
    JS_PROTOTYPE_OBJECT(ListFormatPrototype, ListFormat, Intl.ListFormat);
    GC_DECLARE_ALLOCATOR(ListFormatPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~ListFormatPrototype() override = default;

private:
    explicit ListFormatPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(resolved_options);
    JS_DECLARE_NATIVE_FUNCTION(format);
    JS_DECLARE_NATIVE_FUNCTION(format_to_parts);
};

}
