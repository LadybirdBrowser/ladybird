/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Intl/DurationFormat.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS::Intl {

class DurationFormatPrototype final : public PrototypeObject<DurationFormatPrototype, DurationFormat> {
    JS_PROTOTYPE_OBJECT(DurationFormatPrototype, DurationFormat, Intl.DurationFormat);
    JS_DECLARE_ALLOCATOR(DurationFormatPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~DurationFormatPrototype() override = default;

private:
    explicit DurationFormatPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(format);
    JS_DECLARE_NATIVE_FUNCTION(format_to_parts);
    JS_DECLARE_NATIVE_FUNCTION(resolved_options);
};

}
