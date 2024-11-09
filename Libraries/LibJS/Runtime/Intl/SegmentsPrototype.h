/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Intl/Segments.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS::Intl {

class SegmentsPrototype final : public PrototypeObject<SegmentsPrototype, Segments> {
    JS_PROTOTYPE_OBJECT(SegmentsPrototype, Segments, Segments);
    JS_DECLARE_ALLOCATOR(SegmentsPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~SegmentsPrototype() override = default;

private:
    explicit SegmentsPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(containing);
    JS_DECLARE_NATIVE_FUNCTION(symbol_iterator);
};

}
