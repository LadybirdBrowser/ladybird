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
class IteratorRecord final : public Cell {
    GC_CELL(IteratorRecord, Cell);
    GC_DECLARE_ALLOCATOR(IteratorRecord);

public:
    IteratorRecord(GC::Ptr<Object> iterator, Value next_method, bool done)
        : done(done)
        , iterator(iterator)
        , next_method(next_method)
    {
    }

    bool done { false };      // [[Done]]
    GC::Ptr<Object> iterator; // [[Iterator]]
    Value next_method;        // [[NextMethod]]

private:
    virtual void visit_edges(Cell::Visitor&) override;
};

static_assert(sizeof(IteratorRecord) == 32);

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

class BuiltinIterator {
public:
    virtual ~BuiltinIterator() = default;
    virtual ThrowCompletionOr<void> next(VM&, bool& done, Value& value) = 0;

protected:
    bool m_next_method_was_redefined { false };
};

struct IterationResult {
    ThrowCompletionOr<Value> done;
    ThrowCompletionOr<Value> value;
};
struct IterationDone { };
using IterationResultOrDone = Variant<IterationResult, IterationDone>;

// 7.4.12 IfAbruptCloseIterator ( value, iteratorRecord ), https://tc39.es/ecma262/#sec-ifabruptcloseiterator
#define TRY_OR_CLOSE_ITERATOR(vm, iterator_record, expression)                                                    \
    ({                                                                                                            \
        auto&& _temporary_try_or_close_result = (expression);                                                     \
                                                                                                                  \
        /* 1. Assert: value is a Completion Record. */                                                            \
        /* 2. If value is an abrupt completion, return ? IteratorClose(iteratorRecord, value). */                 \
        if (_temporary_try_or_close_result.is_error()) {                                                          \
            return iterator_close(vm, iterator_record, _temporary_try_or_close_result.release_error());           \
        }                                                                                                         \
                                                                                                                  \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_temporary_try_or_close_result.release_value())>, \
            "Do not return a reference from a fallible expression");                                              \
                                                                                                                  \
        /* 3. Else, set value to ! value. */                                                                      \
        _temporary_try_or_close_result.release_value();                                                           \
    })

ThrowCompletionOr<GC::Ref<IteratorRecord>> get_iterator_direct(VM&, Object&);
ThrowCompletionOr<GC::Ref<IteratorRecord>> get_iterator_from_method(VM&, Value, GC::Ref<FunctionObject>);
ThrowCompletionOr<GC::Ref<IteratorRecord>> get_iterator(VM&, Value, IteratorHint);
ThrowCompletionOr<GC::Ref<IteratorRecord>> get_iterator_flattenable(VM&, Value, PrimitiveHandling);
ThrowCompletionOr<GC::Ref<Object>> iterator_next(VM&, IteratorRecord&, Optional<Value> = {});
ThrowCompletionOr<bool> iterator_complete(VM&, Object& iterator_result);
ThrowCompletionOr<Value> iterator_value(VM&, Object& iterator_result);
ThrowCompletionOr<IterationResultOrDone> iterator_step(VM&, IteratorRecord&);
ThrowCompletionOr<Optional<Value>> iterator_step_value(VM&, IteratorRecord&);
Completion iterator_close(VM&, IteratorRecord const&, Completion);
Completion async_iterator_close(VM&, IteratorRecord const&, Completion);
GC::Ref<Object> create_iterator_result_object(VM&, Value, bool done);
ThrowCompletionOr<GC::RootVector<Value>> iterator_to_list(VM&, IteratorRecord&);
ThrowCompletionOr<void> setter_that_ignores_prototype_properties(VM&, Value this_, Object const& home, PropertyKey const& property, Value value);

using IteratorValueCallback = Function<Optional<Completion>(Value)>;
Completion get_iterator_values(VM&, Value iterable, IteratorValueCallback callback);

}
