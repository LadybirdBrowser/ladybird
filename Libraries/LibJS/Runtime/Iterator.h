/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/RootVector.h>
#include <LibJS/Export.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

struct IteratorRecordImpl {
    bool done { false };      // [[Done]]
    GC::Ptr<Object> iterator; // [[Iterator]]
    Value next_method;        // [[NextMethod]]
};

// 7.4.1 Iterator Records, https://tc39.es/ecma262/#sec-iterator-records
// The record is one of the embedder's cells, and the runtime's operations on it take its fields.
class JS_API IteratorRecord final
    : public Cell
    , public IteratorRecordImpl {
    GC_CELL(IteratorRecord, Cell);
    GC_DECLARE_ALLOCATOR(IteratorRecord);

public:
    IteratorRecord(GC::Ptr<Object> iterator, Value next_method, bool done)
        : IteratorRecordImpl(done, iterator, next_method)
    {
    }

private:
    virtual void visit_edges(Cell::Visitor&) override;
};

enum class IteratorHint {
    Sync,
    Async,
};

JS_API ThrowCompletionOr<IteratorRecordImpl> get_iterator_from_method_impl(VM&, Value, GC::Ref<FunctionObject>);
JS_API ThrowCompletionOr<GC::Ref<IteratorRecord>> get_iterator_from_method(VM&, Value, GC::Ref<FunctionObject>);
JS_API ThrowCompletionOr<IteratorRecordImpl> get_iterator_impl(VM&, Value, IteratorHint);
JS_API ThrowCompletionOr<GC::Ref<IteratorRecord>> get_iterator(VM&, Value, IteratorHint);
JS_API ThrowCompletionOr<GC::Ref<Object>> iterator_next(VM&, IteratorRecordImpl&, Optional<Value> = {});
JS_API ThrowCompletionOr<bool> iterator_complete(VM&, Object& iterator_result);
JS_API ThrowCompletionOr<Value> iterator_value(VM&, Object& iterator_result);
JS_API ThrowCompletionOr<Optional<Value>> iterator_step_value(VM&, IteratorRecordImpl&);
JS_API GC::Ref<Object> create_iterator_result_object(Realm&, Value, bool done);
JS_API GC::Ref<Object> create_iterator_result_object(VM&, Value, bool done);
JS_API ThrowCompletionOr<GC::RootVector<Value>> iterator_to_list(VM&, IteratorRecord&);

}
