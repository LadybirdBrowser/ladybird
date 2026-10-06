/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/IterationDecision.h>
#include <AK/OwnPtr.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <LibGC/Weak.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Embedding/Layout.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/EngineCell.h>
#include <LibJS/Runtime/PropertyAttributes.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

// The shape of an object, which LibJS's users only ask for the realm it belongs to and the prototype it records. Both
// are fields of the runtime's shape.
class JS_API Shape final : public EngineCell {
public:
    Realm& realm() const { return *field_at<Realm*>(JS_LAYOUT_SHAPE_REALM_OFFSET); }

    Object* prototype() { return field_at<Object*>(JS_LAYOUT_SHAPE_PROTOTYPE_OFFSET); }
    Object const* prototype() const { return field_at<Object*>(JS_LAYOUT_SHAPE_PROTOTYPE_OFFSET); }

private:
    static_assert(JS_LAYOUT_SHAPE_REALM_SIZE == sizeof(void*));
    static_assert(JS_LAYOUT_SHAPE_PROTOTYPE_SIZE == sizeof(void*));

    template<typename Field>
    Field field_at(size_t offset) const
    {
        Field field;
        __builtin_memcpy(&field, reinterpret_cast<u8 const*>(this) + offset, sizeof(field));
        return field;
    }
};

}
