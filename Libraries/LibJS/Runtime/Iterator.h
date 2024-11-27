/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

// 7.4.1 Iterator Records, https://tc39.es/ecma262/#sec-iterator-records
class IteratorRecord final : public Object {
    JS_OBJECT(IteratorRecord, Object);
    GC_DECLARE_ALLOCATOR(IteratorRecord);

public:
    IteratorRecord(Realm& realm, GC::Ptr<Object> iterator, Value next_method, bool done)
        : Object(ConstructWithoutPrototypeTag::Tag, realm)
        , iterator(iterator)
        , next_method(next_method)
        , done(done)
    {
    }

    GC::Ptr<Object> iterator; // [[Iterator]]
    Value next_method;        // [[NextMethod]]
    bool done { false };      // [[Done]]

private:
    virtual void visit_edges(Cell::Visitor&) override;
    virtual bool is_iterator_record() const override { return true; }
};

template<>
inline bool Object::fast_is<IteratorRecord>() const { return is_iterator_record(); }

class Iterator : public Object {
    JS_OBJECT(Iterator, Object);
    GC_DECLARE_ALLOCATOR(Iterator);

public:
    static GC::Ref<Iterator> create(Realm&, Object& prototype, GC::Ref<IteratorRecord> iterated);

    IteratorRecord const& iterated() const { return m_iterated; }

private:
    Iterator(Object& prototype, GC::Ref<IteratorRecord> iterated);
    explicit Iterator(Object& prototype);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<IteratorRecord> m_iterated; // [[Iterated]]
};

enum class IteratorHint {
    Sync,
    Async,
};

enum class PrimitiveHandling {
    IterateStringPrimitives,
    RejectPrimitives,
};

ThrowCompletionOr<GC::Ref<IteratorRecord>> get_iterator_direct(VM&, Object&);
ThrowCompletionOr<GC::Ref<IteratorRecord>> get_iterator_from_method(VM&, Value, GC::Ref<FunctionObject>);
ThrowCompletionOr<GC::Ref<IteratorRecord>> get_iterator(VM&, Value, IteratorHint);
ThrowCompletionOr<GC::Ref<IteratorRecord>> get_iterator_flattenable(VM&, Value, PrimitiveHandling);
ThrowCompletionOr<GC::Ref<Object>> iterator_next(VM&, IteratorRecord&, Optional<Value> = {});
ThrowCompletionOr<bool> iterator_complete(VM&, Object& iterator_result);
ThrowCompletionOr<Value> iterator_value(VM&, Object& iterator_result);
ThrowCompletionOr<GC::Ptr<Object>> iterator_step(VM&, IteratorRecord&);
ThrowCompletionOr<Optional<Value>> iterator_step_value(VM&, IteratorRecord&);
Completion iterator_close(VM&, IteratorRecord const&, Completion);
Completion async_iterator_close(VM&, IteratorRecord const&, Completion);
GC::Ref<Object> create_iterator_result_object(VM&, Value, bool done);
ThrowCompletionOr<GC::MarkedVector<Value>> iterator_to_list(VM&, IteratorRecord&);
ThrowCompletionOr<void> setter_that_ignores_prototype_properties(VM&, Value this_, Object const& home, PropertyKey const& property, Value value);

using IteratorValueCallback = Function<Optional<Completion>(Value)>;
Completion get_iterator_values(VM&, Value iterable, IteratorValueCallback callback);

}