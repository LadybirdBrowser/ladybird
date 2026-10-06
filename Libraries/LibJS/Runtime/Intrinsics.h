/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/EngineCell.h>

namespace JS {

// The intrinsics of a realm. The runtime reaches each intrinsic through the realm it belongs to, and creates some of
// them only when they are first needed, so the facade's Intrinsics is the realm's own cell seen through the getters
// that LibJS's users call. Realm::intrinsics() is a cast.
class JS_API Intrinsics final : public EngineCell {
public:
    GC::Ref<Object> object_prototype();
    GC::Ref<Object> function_prototype();
    GC::Ref<Object> array_prototype();
    GC::Ref<Object> string_prototype();
    GC::Ref<Object> error_prototype();
    GC::Ref<Object> iterator_prototype();
    GC::Ref<Object> async_iterator_prototype();

    GC::Ref<ErrorConstructor> error_constructor();
    GC::Ref<PromiseConstructor> promise_constructor();
    GC::Ref<SharedArrayBufferConstructor> shared_array_buffer_constructor();
    GC::Ref<DataViewConstructor> data_view_constructor();

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    GC::Ref<ConstructorName> snake_name##_constructor();
    JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

    GC::Ref<FunctionObject> json_stringify_function() const;

    GC::Ref<ConsoleObject> console_object();

private:
    Realm& realm() const;
};

}
