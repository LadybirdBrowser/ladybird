/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

GC_DEFINE_ALLOCATOR(IteratorRecord);

static_assert(to_underlying(IteratorHint::Sync) == JS_ITERATOR_HINT_SYNC);
static_assert(to_underlying(IteratorHint::Async) == JS_ITERATOR_HINT_ASYNC);

void IteratorRecord::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(iterator);
    visitor.visit(next_method);
}

// The runtime's operations on an Iterator Record take its fields, and update [[Done]] in them.
class IteratorRecordFields {
public:
    explicit IteratorRecordFields(IteratorRecordImpl const& record)
        : m_fields {
            .done = record.done,
            .iterator = record.iterator ? object_to_abi(*record.iterator) : nullptr,
            .next_method = value_to_abi(record.next_method),
        }
    {
    }

    JSIteratorRecordFields* abi() { return &m_fields; }

    void write_done_to(IteratorRecordImpl& record) const { record.done = m_fields.done; }

private:
    JSIteratorRecordFields m_fields;
};

static IteratorRecordImpl iterator_record_from_abi(JSIteratorRecordFields const& fields)
{
    return IteratorRecordImpl {
        .done = fields.done,
        .iterator = &object_from_abi(fields.iterator),
        .next_method = value_from_abi(fields.next_method),
    };
}

static GC::Ref<IteratorRecord> create_iterator_record(VM& vm, IteratorRecordImpl const& record)
{
    return vm.heap().allocate<IteratorRecord>(record.iterator, record.next_method, record.done);
}

// 7.4.3 GetIteratorFromMethod ( obj, method ), https://tc39.es/ecma262/#sec-getiteratorfrommethod
ThrowCompletionOr<IteratorRecordImpl> get_iterator_from_method_impl(VM& vm, Value object, GC::Ref<FunctionObject> method)
{
    JSIteratorRecordFields fields {};
    TRY(completion_from_abi<void>(js_iterator_get_fields_from_method(vm_to_abi(vm), value_to_abi(object), object_to_abi(*method), &fields)));
    return iterator_record_from_abi(fields);
}

ThrowCompletionOr<GC::Ref<IteratorRecord>> get_iterator_from_method(VM& vm, Value object, GC::Ref<FunctionObject> method)
{
    return create_iterator_record(vm, TRY(get_iterator_from_method_impl(vm, object, method)));
}

// 7.4.4 GetIterator ( obj, kind ), https://tc39.es/ecma262/#sec-getiterator
ThrowCompletionOr<IteratorRecordImpl> get_iterator_impl(VM& vm, Value object, IteratorHint kind)
{
    JSIteratorRecordFields fields {};
    TRY(completion_from_abi<void>(js_iterator_get_fields(vm_to_abi(vm), value_to_abi(object), to_underlying(kind), &fields)));
    return iterator_record_from_abi(fields);
}

ThrowCompletionOr<GC::Ref<IteratorRecord>> get_iterator(VM& vm, Value object, IteratorHint kind)
{
    return create_iterator_record(vm, TRY(get_iterator_impl(vm, object, kind)));
}

// 7.4.6 IteratorNext ( iteratorRecord [ , value ] ), https://tc39.es/ecma262/#sec-iteratornext
ThrowCompletionOr<GC::Ref<Object>> iterator_next(VM& vm, IteratorRecordImpl& iterator_record, Optional<Value> value)
{
    IteratorRecordFields fields { iterator_record };
    JSValue value_to_send = value.has_value() ? value_to_abi(*value) : 0;
    auto result = js_iterator_fields_next(vm_to_abi(vm), fields.abi(), value.has_value() ? &value_to_send : nullptr);
    fields.write_done_to(iterator_record);
    return completion_from_abi<GC::Ref<Object>>(result);
}

// 7.4.7 IteratorComplete ( iteratorResult ), https://tc39.es/ecma262/#sec-iteratorcomplete
ThrowCompletionOr<bool> iterator_complete(VM& vm, Object& iterator_result)
{
    return completion_from_abi<bool>(js_iterator_complete(vm_to_abi(vm), object_to_abi(iterator_result)));
}

// 7.4.8 IteratorValue ( iteratorResult ), https://tc39.es/ecma262/#sec-iteratorvalue
ThrowCompletionOr<Value> iterator_value(VM& vm, Object& iterator_result)
{
    return completion_from_abi<Value>(js_iterator_value(vm_to_abi(vm), object_to_abi(iterator_result)));
}

// 7.4.10 IteratorStepValue ( iteratorRecord ), https://tc39.es/ecma262/#sec-iteratorstepvalue
ThrowCompletionOr<Optional<Value>> iterator_step_value(VM& vm, IteratorRecordImpl& iterator_record)
{
    IteratorRecordFields fields { iterator_record };
    JSValue value = 0;
    auto result = js_iterator_fields_step_value(vm_to_abi(vm), fields.abi(), &value);
    fields.write_done_to(iterator_record);
    if (!TRY(completion_from_abi<bool>(result)))
        return OptionalNone {};
    return value_from_abi(value);
}

// 7.4.15 CreateIteratorResultObject ( value, done ), https://tc39.es/ecma262/#sec-createiterresultobject
GC::Ref<Object> create_iterator_result_object(Realm& realm, Value value, bool done)
{
    return object_from_abi(js_iterator_create_result_object(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), value_to_abi(value), done));
}

// 7.4.15 CreateIteratorResultObject ( value, done ), https://tc39.es/ecma262/#sec-createiterresultobject
GC::Ref<Object> create_iterator_result_object(VM& vm, Value value, bool done)
{
    return create_iterator_result_object(*vm.current_realm(), value, done);
}

// 7.4.17 IteratorToList ( iteratorRecord ), https://tc39.es/ecma262/#sec-iteratortolist
ThrowCompletionOr<GC::RootVector<Value>> iterator_to_list(VM& vm, IteratorRecord& iterator_record)
{
    GC::RootVector<Value> values;
    JSValueSink sink {
        .context = &values,
        .append = [](void* context, JSValue value) {
            static_cast<GC::RootVector<Value>*>(context)->append(value_from_abi(value));
        },
    };

    IteratorRecordFields fields { iterator_record };
    auto result = js_iterator_fields_to_list(vm_to_abi(vm), fields.abi(), &sink);
    fields.write_done_to(iterator_record);
    TRY(completion_from_abi<void>(result));
    return values;
}

}
