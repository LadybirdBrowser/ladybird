/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Intl/SegmentIterator.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS::Intl {

class SegmentIteratorPrototype final : public PrototypeObject<SegmentIteratorPrototype, SegmentIterator> {
    JS_PROTOTYPE_OBJECT(SegmentIteratorPrototype, SegmentIterator, SegmentIterator);
    JS_DECLARE_ALLOCATOR(SegmentIteratorPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~SegmentIteratorPrototype() override = default;

private:
    explicit SegmentIteratorPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(next);
};

}
