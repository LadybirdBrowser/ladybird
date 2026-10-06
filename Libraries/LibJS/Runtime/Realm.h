/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Function.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/StringView.h>
#include <AK/Traits.h>
#include <AK/Weakable.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibJS/Embedding/Layout.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/EngineCell.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

// 9.3 Realms, https://tc39.es/ecma262/#realm-record
// The runtime keeps a realm alive only while something references it, and an execution context references its realm
// only while it is on the execution context stack, so the embedder keeps the realms it creates alive itself, as the
// settings objects of HTML do.
class JS_API Realm final : public EngineCell {
public:
    // Creates one of the embedder's own cells, which LibGC's heap allocates, and initializes it in this realm. The
    // runtime's cells come from the runtime.
    template<typename T, typename... Args>
    GC::Ref<T> create(Args&&... args)
    {
        static_assert(!IsBaseOf<GC::ForeignCell, T>, "Realm::create() allocates C++ cells; the runtime creates its own");
        auto object = heap().allocate<T>(forward<Args>(args)...);
        if constexpr (IsBaseOf<Cell, T>)
            static_cast<Cell*>(object.ptr())->initialize(*this);
        return *object;
    }

    static ThrowCompletionOr<NonnullOwnPtr<ExecutionContext>> initialize_host_defined_realm(VM&, Function<GC::Ref<Object>(Realm&)> create_global_object, Function<GC::Ref<Object>(Realm&)> create_global_this_value);

    [[nodiscard]] Object& global_object() const { return *field_at<Object*>(JS_LAYOUT_REALM_GLOBAL_OBJECT_OFFSET); }
    [[nodiscard]] GlobalEnvironment& global_environment() const { return *field_at<GlobalEnvironment*>(JS_LAYOUT_REALM_GLOBAL_ENVIRONMENT_OFFSET); }

    [[nodiscard]] Intrinsics const& intrinsics() const { return *reinterpret_cast<Intrinsics const*>(this); }
    [[nodiscard]] Intrinsics& intrinsics() { return *reinterpret_cast<Intrinsics*>(this); }

    GC::Ptr<GC::Cell> host_defined() { return field_at<GC::Cell*>(JS_LAYOUT_REALM_HOST_DEFINED_OFFSET); }
    GC::Ptr<GC::Cell const> host_defined() const { return field_at<GC::Cell*>(JS_LAYOUT_REALM_HOST_DEFINED_OFFSET); }

    // The realm keeps the cell alive.
    void set_host_defined(GC::Ptr<GC::Cell>);

private:
    static_assert(JS_LAYOUT_REALM_GLOBAL_OBJECT_SIZE == sizeof(void*));
    static_assert(JS_LAYOUT_REALM_GLOBAL_ENVIRONMENT_SIZE == sizeof(void*));
    static_assert(JS_LAYOUT_REALM_HOST_DEFINED_SIZE == sizeof(void*));

    template<typename Field>
    Field field_at(size_t offset) const
    {
        static_assert(sizeof(Field) == sizeof(void*));
        Field field;
        __builtin_memcpy(&field, reinterpret_cast<u8 const*>(this) + offset, sizeof(field));
        return field;
    }
};

}
