/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/AsyncFromSyncIteratorPrototype.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

GC_DEFINE_ALLOCATOR(Iterator);
GC_DEFINE_ALLOCATOR(IteratorRecord);

GC::Ref<Iterator> Iterator::create(Realm& realm, Object& prototype, GC::Ref<IteratorRecord> iterated)
{
    return realm.create<Iterator>(prototype, move(iterated));
}

Iterator::Iterator(Object& prototype, GC::Ref<IteratorRecord> iterated)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_iterated(move(iterated))
{
}

Iterator::Iterator(Object& prototype)
    : Iterator(prototype, prototype.heap().allocate<IteratorRecord>(nullptr, js_undefined(), false))
{
}

// 7.4.2 GetIteratorDirect ( obj ), https://tc39.es/ecma262/#sec-getiteratordirect
ThrowCompletionOr<GC::Ref<IteratorRecord>> get_iterator_direct(VM& vm, Object& object)
{
    // 1. Let nextMethod be ? Get(obj, "next").
    auto next_method = TRY(object.get(vm.names.next));

    // 2. Let iteratorRecord be Record { [[Iterator]]: obj, [[NextMethod]]: nextMethod, [[Done]]: false }.
    // 3. Return iteratorRecord.
    return vm.heap().allocate<IteratorRecord>(object, next_method, false);
}

// 7.4.3 GetIteratorFromMethod ( obj, method ), https://tc39.es/ecma262/#sec-getiteratorfrommethod
ThrowCompletionOr<GC::Ref<IteratorRecord>> get_iterator_from_method(VM& vm, Value object, GC::Ref<FunctionObject> method)
{
    // 1. Let iterator be ? Call(method, obj).
    auto iterator = TRY(call(vm, *method, object));

    // 2. If iterator is not an Object, throw a TypeError exception.
    if (!iterator.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotIterable, object.to_string_without_side_effects());

    // 3. Let nextMethod be ? Get(iterator, "next").
    auto next_method = TRY(iterator.get(vm, vm.names.next));

    // 4. Let iteratorRecord be the Iterator Record { [[Iterator]]: iterator, [[NextMethod]]: nextMethod, [[Done]]: false }.
    auto iterator_record = vm.heap().allocate<IteratorRecord>(iterator.as_object(), next_method, false);

    // 5. Return iteratorRecord.
    return iterator_record;
}

// 7.4.4 GetIterator ( obj, kind ), https://tc39.es/ecma262/#sec-getiterator
ThrowCompletionOr<GC::Ref<IteratorRecord>> get_iterator(VM& vm, Value object, IteratorHint kind)
{
    GC::Ptr<FunctionObject> method;

    // 1. If kind is async, then
    if (kind == IteratorHint::Async) {
        // a. Let method be ? GetMethod(obj, @@asyncIterator).
        method = TRY(object.get_method(vm, vm.well_known_symbol_async_iterator()));

        // b. If method is undefined, then
        if (!method) {
            // i. Let syncMethod be ? GetMethod(obj, @@iterator).
            auto sync_method = TRY(object.get_method(vm, vm.well_known_symbol_iterator()));

            // ii. If syncMethod is undefined, throw a TypeError exception.
            if (!sync_method)
                return vm.throw_completion<TypeError>(ErrorType::NotIterable, object.to_string_without_side_effects());

            // iii. Let syncIteratorRecord be ? GetIteratorFromMethod(obj, syncMethod).
            auto sync_iterator_record = TRY(get_iterator_from_method(vm, object, *sync_method));

            // iv. Return CreateAsyncFromSyncIterator(syncIteratorRecord).
            return create_async_from_sync_iterator(vm, sync_iterator_record);
        }
    }
    // 2. Else,
    else {
        // a. Let method be ? GetMethod(obj, @@iterator).
        method = TRY(object.get_method(vm, vm.well_known_symbol_iterator()));
    }

    // 3. If method is undefined, throw a TypeError exception.
    if (!method)
        return vm.throw_completion<TypeError>(ErrorType::NotIterable, object.to_string_without_side_effects());

    // 4. Return ? GetIteratorFromMethod(obj, method).
    return TRY(get_iterator_from_method(vm, object, *method));
}

// 7.4.5 GetIteratorFlattenable ( obj, primitiveHandling ), https://tc39.es/ecma262/#sec-getiteratorflattenable
ThrowCompletionOr<GC::Ref<IteratorRecord>> get_iterator_flattenable(VM& vm, Value object, PrimitiveHandling primitive_handling)
{
    // 1. If obj is not an Object, then
    if (!object.is_object()) {
        // a. If primitiveHandling is reject-primitives, throw a TypeError exception.
        if (primitive_handling == PrimitiveHandling::RejectPrimitives)
            return vm.throw_completion<TypeError>(ErrorType::NotAnObject, object.to_string_without_side_effects());

        // b. Assert: primitiveHandling is iterate-string-primitives.
        ASSERT(primitive_handling == PrimitiveHandling::IterateStringPrimitives);

        // c. If obj is not a String, throw a TypeError exception.
        if (!object.is_string())
            return vm.throw_completion<TypeError>(ErrorType::NotAString, object.to_string_without_side_effects());
    }

    // 2. Let method be ? GetMethod(obj, %Symbol.iterator%).
    auto method = TRY(object.get_method(vm, vm.well_known_symbol_iterator()));

    Value iterator;

    // 3. If method is undefined, then
    if (!method) {
        // a. Let iterator be obj.
        iterator = object;
    }
    // 4. Else,
    else {
        // a. Let iterator be ? Call(method, obj).
        iterator = TRY(call(vm, method, object));
    }

    // 5. If iterator is not an Object, throw a TypeError exception.
    if (!iterator.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, iterator.to_string_without_side_effects());

    // 6. Return ? GetIteratorDirect(iterator).
    return TRY(get_iterator_direct(vm, iterator.as_object()));
}

// 7.4.6 IteratorNext ( iteratorRecord [ , value ] ), https://tc39.es/ecma262/#sec-iteratornext
ThrowCompletionOr<GC::Ref<Object>> iterator_next(VM& vm, IteratorRecord& iterator_record, Optional<Value> value)
{
    auto result = [&]() {
        // 1. If value is not present, then
        if (!value.has_value()) {
            // a. Let result be Completion(Call(iteratorRecord.[[NextMethod]], iteratorRecord.[[Iterator]])).
            return call(vm, iterator_record.next_method, iterator_record.iterator);
        }
        // 2. Else,
        else {
            // a. Let result be Completion(Call(iteratorRecord.[[NextMethod]], iteratorRecord.[[Iterator]], « value »)).
            return call(vm, iterator_record.next_method, iterator_record.iterator, *value);
        }
    }();

    // 3. If result is a throw completion, then
    if (result.is_throw_completion()) {
        // a. Set iteratorRecord.[[Done]] to true.
        iterator_record.done = true;

        // b. Return ? result.
        return result.release_error();
    }

    // 4. Set result to ! result.
    auto result_value = result.release_value();

    // 5. If result is not an Object, then
    if (!result_value.is_object()) {
        // a. Set iteratorRecord.[[Done]] to true.
        iterator_record.done = true;

        // b. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::IterableNextBadReturn);
    }

    // 6. Return result.
    return result_value.as_object();
}

// 7.4.7 IteratorComplete ( iteratorResult ), https://tc39.es/ecma262/#sec-iteratorcomplete
ThrowCompletionOr<bool> iterator_complete(VM& vm, Object& iterator_result)
{
    // 1. Return ToBoolean(? Get(iterResult, "done")).
    return TRY(iterator_result.get(vm.names.done)).to_boolean();
}

// 7.4.8 IteratorValue ( iteratorResult ), https://tc39.es/ecma262/#sec-iteratorvalue
ThrowCompletionOr<Value> iterator_value(VM& vm, Object& iterator_result)
{
    // 1. Return ? Get(iterResult, "value").
    return TRY(iterator_result.get(vm.names.value));
}

// 7.4.9 IteratorStep ( iteratorRecord ), https://tc39.es/ecma262/#sec-iteratorstep
ThrowCompletionOr<GC::Ptr<Object>> iterator_step(VM& vm, IteratorRecord& iterator_record)
{
    // 1. Let result be ? IteratorNext(iteratorRecord).
    auto result = TRY(iterator_next(vm, iterator_record));

    // 2. Let done be Completion(IteratorComplete(result)).
    auto done = iterator_complete(vm, result);

    // 3. If done is a throw completion, then
    if (done.is_throw_completion()) {
        // a. Set iteratorRecord.[[Done]] to true.
        iterator_record.done = true;

        // b. Return ? done.
        return done.release_error();
    }

    // 4. Set done to ! done.
    auto done_value = done.release_value();

    // 5. If done is true, then
    if (done_value) {
        // a. Set iteratorRecord.[[Done]] to true.
        iterator_record.done = true;

        // b. Return DONE.
        return nullptr;
    }

    // 6. Return result.
    return result;
}

// 7.4.10 IteratorStepValue ( iteratorRecord ), https://tc39.es/ecma262/#sec-iteratorstepvalue
ThrowCompletionOr<Optional<Value>> iterator_step_value(VM& vm, IteratorRecord& iterator_record)
{
    // 1. Let result be ? IteratorStep(iteratorRecord).
    auto result = TRY(iterator_step(vm, iterator_record));

    // 2. If result is done, then
    if (!result) {
        // a. Return DONE.
        return OptionalNone {};
    }

    // 3. Let value be Completion(IteratorValue(result)).
    auto value = iterator_value(vm, *result);

    // 4. If value is a throw completion, then
    if (value.is_throw_completion()) {
        // a. Set iteratorRecord.[[Done]] to true.
        iterator_record.done = true;
    }

    // 5. Return ? value.
    return TRY(value);
}

// 7.4.11 IteratorClose ( iteratorRecord, completion , https://tc39.es/ecma262/#sec-iteratorclose
// 7.4.13 AsyncIteratorClose ( iteratorRecord, completion ), https://tc39.es/ecma262/#sec-asynciteratorclose
// NOTE: These only differ in that async awaits the inner value after the call.
static Completion iterator_close_impl(VM& vm, IteratorRecord const& iterator_record, Completion completion, IteratorHint iterator_hint)
{
    // 1. Assert: Type(iteratorRecord.[[Iterator]]) is Object.

    // 2. Let iterator be iteratorRecord.[[Iterator]].
    auto iterator = iterator_record.iterator;

    // 3. Let innerResult be Completion(GetMethod(iterator, "return")).
    auto inner_result = ThrowCompletionOr<Value> { js_undefined() };
    auto get_method_result = Value(iterator).get_method(vm, vm.names.return_);
    if (get_method_result.is_error())
        inner_result = get_method_result.release_error();

    // 4. If innerResult.[[Type]] is normal, then
    if (!inner_result.is_error()) {
        // a. Let return be innerResult.[[Value]].
        auto return_method = get_method_result.value();

        // b. If return is undefined, return ? completion.
        if (!return_method)
            return completion;

        // c. Set innerResult to Completion(Call(return, iterator)).
        inner_result = call(vm, return_method, iterator);

        // Note: If this is AsyncIteratorClose perform one extra step.
        if (iterator_hint == IteratorHint::Async && !inner_result.is_error()) {
            // d. If innerResult.[[Type]] is normal, set innerResult to Completion(Await(innerResult.[[Value]])).
            inner_result = await(vm, inner_result.value());
        }
    }

    // 5. If completion.[[Type]] is throw, return ? completion.
    if (completion.is_error())
        return completion;

    // 6. If innerResult.[[Type]] is throw, return ? innerResult.
    if (inner_result.is_throw_completion())
        return inner_result;

    // 7. If Type(innerResult.[[Value]]) is not Object, throw a TypeError exception.
    if (!inner_result.value().is_object())
        return vm.throw_completion<TypeError>(ErrorType::IterableReturnBadReturn);

    // 8. Return ? completion.
    return completion;
}

// 7.4.11 IteratorClose ( iteratorRecord, completion , https://tc39.es/ecma262/#sec-iteratorclose
Completion iterator_close(VM& vm, IteratorRecord const& iterator_record, Completion completion)
{
    return iterator_close_impl(vm, iterator_record, move(completion), IteratorHint::Sync);
}

// 7.4.13 AsyncIteratorClose ( iteratorRecord, completion ), https://tc39.es/ecma262/#sec-asynciteratorclose
Completion async_iterator_close(VM& vm, IteratorRecord const& iterator_record, Completion completion)
{
    return iterator_close_impl(vm, iterator_record, move(completion), IteratorHint::Async);
}

// 7.4.14 CreateIteratorResultObject ( value, done ), https://tc39.es/ecma262/#sec-createiterresultobject
GC::Ref<Object> create_iterator_result_object(VM& vm, Value value, bool done)
{
    auto& realm = *vm.current_realm();

    // 1. Let obj be OrdinaryObjectCreate(%Object.prototype%).
    auto object = Object::create_with_premade_shape(realm.intrinsics().iterator_result_object_shape());

    // 2. Perform ! CreateDataPropertyOrThrow(obj, "value", value).
    object->put_direct(realm.intrinsics().iterator_result_object_value_offset(), value);

    // 3. Perform ! CreateDataPropertyOrThrow(obj, "done", done).
    object->put_direct(realm.intrinsics().iterator_result_object_done_offset(), Value(done));

    // 4. Return obj.
    return object;
}

// 7.4.16 IteratorToList ( iteratorRecord ), https://tc39.es/ecma262/#sec-iteratortolist
ThrowCompletionOr<GC::RootVector<Value>> iterator_to_list(VM& vm, IteratorRecord& iterator_record)
{
    // 1. Let values be a new empty List.
    GC::RootVector<Value> values(vm.heap());

    // 2. Repeat,
    while (true) {
        // a. Let next be ? IteratorStepValue(iteratorRecord).
        auto next = TRY(iterator_step_value(vm, iterator_record));

        // b. If next is DONE, then
        if (!next.has_value()) {
            // i. Return values.
            return values;
        }

        // c. Append next to values.
        values.append(next.release_value());
    }
}

// 7.3.36 SetterThatIgnoresPrototypeProperties ( thisValue, home, p, v ), https://tc39.es/ecma262/#sec-SetterThatIgnoresPrototypeProperties
ThrowCompletionOr<void> setter_that_ignores_prototype_properties(VM& vm, Value this_, Object const& home, PropertyKey const& property, Value value)
{
    // 1. If this is not an Object, then
    if (!this_.is_object()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, this_);
    }

    auto& this_object = this_.as_object();

    // 2. If this is home, then
    if (&this_object == &home) {
        // a. NOTE: Throwing here emulates assignment to a non-writable data property on the home object in strict mode code.
        // b. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::DescWriteNonWritable, this_);
    }

    // 3. Let desc be ? this.[[GetOwnProperty]](p).
    auto desc = TRY(this_object.internal_get_own_property(property));

    // 4. If desc is undefined, then
    if (!desc.has_value()) {
        // a. Perform ? CreateDataPropertyOrThrow(this, p, v).
        TRY(this_object.create_data_property_or_throw(property, value));
    }
    // 5. Else,
    else {
        // a. Perform ? Set(this, p, v, true).
        TRY(this_object.set(property, value, Object::ShouldThrowExceptions::Yes));
    }

    // 6. Return unused.
    return {};
}

// Non-standard
Completion get_iterator_values(VM& vm, Value iterable, IteratorValueCallback callback)
{
    auto iterator_record = TRY(get_iterator(vm, iterable, IteratorHint::Sync));

    while (true) {
        auto next = TRY(iterator_step_value(vm, iterator_record));
        if (!next.has_value())
            return {};

        if (auto completion = callback(next.release_value()); completion.has_value())
            return iterator_close(vm, iterator_record, completion.release_value());
    }
}

void Iterator::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_iterated);
}

void IteratorRecord::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(iterator);
    visitor.visit(next_method);
}

}
