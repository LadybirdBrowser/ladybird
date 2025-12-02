/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/IteratorConstructor.h>
#include <LibJS/Runtime/IteratorHelper.h>
#include <LibJS/Runtime/IteratorPrototype.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

GC_DEFINE_ALLOCATOR(IteratorConstructor);

// 27.1.3.1 The Iterator Constructor, https://tc39.es/ecma262/#sec-iterator-constructor
IteratorConstructor::IteratorConstructor(Realm& realm)
    : Base(realm.vm().names.Iterator.as_string(), realm.intrinsics().function_prototype())
{
}

void IteratorConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 27.1.3.2.3 Iterator.prototype Iterator.prototype, https://tc39.es/ecma262/#sec-iterator.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().iterator_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.concat, concat, 0, attr);
    define_native_function(realm, vm.names.from, from, 1, attr);

    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);
}

// 27.1.3.1.1 Iterator ( ), https://tc39.es/ecma262/#sec-iterator
ThrowCompletionOr<Value> IteratorConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined or the active function object, throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Iterator");
}

// 27.1.3.1.1 Iterator ( ), https://tc39.es/ecma262/#sec-iterator
ThrowCompletionOr<GC::Ref<Object>> IteratorConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined or the active function object, throw a TypeError exception.
    if (&new_target == this)
        return vm.throw_completion<TypeError>(ErrorType::ClassIsAbstract, "Iterator");

    // 2. Return ? OrdinaryCreateFromConstructor(NewTarget, "%Iterator.prototype%").
    return TRY(ordinary_create_from_constructor<Iterator>(vm, new_target, &Intrinsics::iterator_prototype));
}

class ConcatIterator : public Cell {
    GC_CELL(ConcatIterator, Cell);
    GC_DECLARE_ALLOCATOR(ConcatIterator);

    void append_iterable(GC::Ref<FunctionObject> open_method, GC::Ref<Object> iterable)
    {
        m_iterables.empend(open_method, iterable);
    }

    ThrowCompletionOr<IteratorHelper::IterationResult> next(VM& vm, IteratorHelper& iterator)
    {
        if (m_inner_iterator)
            return inner_next(vm, iterator);
        return outer_next(vm, iterator);
    }

    // NB: This implements step 3.a.v.3.b of Iterator.concat.
    ThrowCompletionOr<Value> on_abrupt_completion(VM& vm, Completion const& completion) const
    {
        VERIFY(m_inner_iterator);

        // b. If completion is an abrupt completion, then
        //     i. Return ? IteratorClose(iteratorRecord, completion).
        return TRY(iterator_close(vm, *m_inner_iterator, completion));
    }

public:
    ConcatIterator() = default;

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);

        for (auto const& iterable : m_iterables) {
            visitor.visit(iterable.open_method);
            visitor.visit(iterable.iterable);
        }

        visitor.visit(m_inner_iterator);
    }

    ThrowCompletionOr<IteratorHelper::IterationResult> outer_next(VM& vm, IteratorHelper& iterator)
    {
        // a. For each Record iterable of iterables, do
        if (m_index < m_iterables.size()) {
            auto iterable = m_iterables[m_index++];

            // i. Let iter be ? Call(iterable.[[OpenMethod]], iterable.[[Iterable]]).
            auto iter = TRY(JS::call(vm, *iterable.open_method, iterable.iterable));

            // ii. If iter is not an Object, throw a TypeError exception.
            if (!iter.is_object())
                return vm.throw_completion<TypeError>(ErrorType::NotAnObject, iter);

            // iii. Let iteratorRecord be ? GetIteratorDirect(iter).
            auto iterator_record = TRY(get_iterator_direct(vm, iter.as_object()));

            // iv. Let innerAlive be true.
            m_inner_iterator = iterator_record;

            // v. Repeat, while innerAlive is true,
            return inner_next(vm, iterator);
        }

        // b. Return ReturnCompletion(undefined).
        return IteratorHelper::IterationResult { js_undefined(), true };
    }

    ThrowCompletionOr<IteratorHelper::IterationResult> inner_next(VM& vm, IteratorHelper& iterator)
    {
        VERIFY(m_inner_iterator);

        // 1. Let innerValue be ? IteratorStepValue(iteratorRecord).
        auto inner_value = TRY(iterator_step_value(vm, *m_inner_iterator));

        // 2. If innerValue is DONE, then
        if (!inner_value.has_value()) {
            // a. Set innerAlive to false.
            m_inner_iterator = nullptr;

            return outer_next(vm, iterator);
        }
        // 3. Else,
        else {
            // a. Let completion be Completion(Yield(innerValue)).
            // NB: Step b is implemented via on_abrupt_completion.
            return IteratorHelper::IterationResult { *inner_value, false };
        }
    }

    struct Iterable {
        GC::Ref<FunctionObject> open_method;
        GC::Ref<Object> iterable;
    };
    Vector<Iterable> m_iterables;
    size_t m_index { 0 };

    GC::Ptr<IteratorRecord> m_inner_iterator;
};

GC_DEFINE_ALLOCATOR(ConcatIterator);

// 27.1.3.2.1 Iterator.concat ( ...items ), https://tc39.es/ecma262/#sec-iterator.concat
JS_DEFINE_NATIVE_FUNCTION(IteratorConstructor::concat)
{
    static Bytecode::PropertyLookupCache cache;
    auto& realm = *vm.current_realm();

    // 1. Let iterables be a new empty List.
    auto iterables = realm.create<ConcatIterator>();

    // 2. For each element item of items, do
    for (size_t i = 0; i < vm.argument_count(); ++i) {
        auto item = vm.argument(i);

        // a. If item is not an Object, throw a TypeError exception.
        if (!item.is_object())
            return vm.throw_completion<TypeError>(ErrorType::NotAnObject, item);

        // b. Let method be ? GetMethod(item, %Symbol.iterator%).
        auto method = TRY(item.get_method(vm, vm.well_known_symbol_iterator(), cache));

        // c. If method is undefined, throw a TypeError exception.
        if (!method)
            return vm.throw_completion<TypeError>(ErrorType::NotIterable, item);

        // d. Append the Record { [[OpenMethod]]: method, [[Iterable]]: item } to iterables.
        iterables->append_iterable(*method, item.as_object());
    }

    // 3. Let closure be a new Abstract Closure with no parameters that captures iterables and performs the following steps when called:
    auto closure = GC::create_function(realm.heap(), [iterables](VM& vm, IteratorHelper& iterator) {
        return iterables->next(vm, iterator);
    });

    auto abrupt_closure = GC::create_function(realm.heap(), [iterables](VM& vm, Completion const& completion) -> ThrowCompletionOr<Value> {
        return iterables->on_abrupt_completion(vm, completion);
    });

    // 4. Let gen be CreateIteratorFromClosure(closure, "Iterator Helper", %IteratorHelperPrototype%, « [[UnderlyingIterators]] »).
    // 5. Set gen.[[UnderlyingIterators]] to a new empty List.
    auto gen = IteratorHelper::create(realm, {}, closure, abrupt_closure);

    // 6. Return gen.
    return gen;
}

// 27.1.3.2.2 Iterator.from ( O ), https://tc39.es/ecma262/#sec-iterator.from
JS_DEFINE_NATIVE_FUNCTION(IteratorConstructor::from)
{
    auto& realm = *vm.current_realm();

    auto object = vm.argument(0);

    // 1. Let iteratorRecord be ? GetIteratorFlattenable(O, iterate-string-primitives).
    auto iterator_record = TRY(get_iterator_flattenable(vm, object, PrimitiveHandling::IterateStringPrimitives));

    // 2. Let hasInstance be ? OrdinaryHasInstance(%Iterator%, iteratorRecord.[[Iterator]]).
    auto has_instance = TRY(ordinary_has_instance(vm, iterator_record->iterator, realm.intrinsics().iterator_constructor()));

    // 3. If hasInstance is true, then
    if (has_instance.is_boolean() && has_instance.as_bool()) {
        // a. Return iteratorRecord.[[Iterator]].
        return iterator_record->iterator;
    }

    // 4. Let wrapper be OrdinaryObjectCreate(%WrapForValidIteratorPrototype%, « [[Iterated]] »).
    // 5. Set wrapper.[[Iterated]] to iteratorRecord.
    auto wrapper = Iterator::create(realm, realm.intrinsics().wrap_for_valid_iterator_prototype(), iterator_record);

    // 6. Return wrapper.
    return wrapper;
}

}
