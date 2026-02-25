/*
 * Copyright (c) 2023-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
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
    define_native_function(realm, vm.names.zip, zip, 1, attr);

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

enum class ZipMode {
    Shortest,
    Longest,
    Strict,
};

class ZipIterator : public Cell {
    GC_CELL(ZipIterator, Cell);
    GC_DECLARE_ALLOCATOR(ZipIterator);

public:
    using FinishResults = GC::Function<Value(Realm&, ReadonlySpan<Value>)>;

    ThrowCompletionOr<IteratorHelper::IterationResult> next(VM& vm)
    {
        // a. If iterCount = 0, return ReturnCompletion(undefined).
        if (m_iterators.is_empty())
            return IteratorHelper::IterationResult { js_undefined(), true };

        // b. Repeat,

        // i. Let results be a new empty List.
        GC::RootVector<Value> results { vm.heap() };

        // ii. Assert: openIters is not empty.
        VERIFY(!m_open_iterators.is_empty());

        // iii. For each integer i such that 0 ≤ i < iterCount, in ascending order, do
        for (auto [i, iterator] : enumerate(m_iterators)) {
            Optional<Value> result;

            // 1. Let iter be iters[i].
            // 2. If iter is null, then
            if (!iterator) {
                // a. Assert: mode is "longest".
                VERIFY(m_mode == ZipMode::Longest);

                // b. Let result be padding[i].
                result = m_padding[i];
            }
            // 3. Else,
            else {
                // a. Let result be Completion(IteratorStepValue(iter)).
                auto step_value_result = iterator_step_value(vm, *iterator);

                // b. If result is an abrupt completion, then
                if (step_value_result.is_throw_completion()) {
                    // i. Remove iter from openIters.
                    remove_iterator_from_open_iterators(iterator);

                    // ii. Return ? IteratorCloseAll(openIters, result).
                    return TRY(close_all_open_iterators(vm, step_value_result.release_error()));
                }

                // c. Set result to ! result.
                result = step_value_result.release_value();

                // d. If result is DONE, then
                if (!result.has_value()) {
                    // i. Remove iter from openIters.
                    remove_iterator_from_open_iterators(iterator);

                    switch (m_mode) {
                    // ii. If mode is "shortest", then
                    case ZipMode::Shortest:
                        // i. Return ? IteratorCloseAll(openIters, ReturnCompletion(undefined)).
                        return TRY(close_all_open_iterators(vm, js_undefined()));

                    // iii. Else if mode is "strict", then
                    case ZipMode::Strict:
                        // i. If i ≠ 0, then
                        if (i != 0) {
                            // i. Return ? IteratorCloseAll(openIters, ThrowCompletion(a newly created TypeError object)).
                            return TRY(close_all_open_iterators(vm, vm.throw_completion<TypeError>(ErrorType::ZipIteratorNotEnoughResults)));
                        }

                        // ii. For each integer k such that 1 ≤ k < iterCount, in ascending order, do
                        for (auto iterator_k : m_iterators.span().slice(1)) {
                            // i. Assert: iters[k] is not null.
                            VERIFY(iterator_k);

                            // ii. Let open be Completion(IteratorStep(iters[k])).
                            auto step_result = iterator_step(vm, *iterator_k);

                            // iii. If open is an abrupt completion, then
                            if (step_result.is_throw_completion()) {
                                // i. Remove iters[k] from openIters.
                                remove_iterator_from_open_iterators(iterator_k);

                                // ii. Return ? IteratorCloseAll(openIters, open).
                                return TRY(close_all_open_iterators(vm, step_result.release_error()));
                            }

                            // iv. Set open to ! open.
                            auto open = step_result.release_value();

                            // v. If open is DONE, then
                            if (open.has<IterationDone>()) {
                                // i. Remove iters[k] from openIters.
                                remove_iterator_from_open_iterators(iterator_k);
                            }
                            // vi. Else,
                            else {
                                // i. Return ? IteratorCloseAll(openIters, ThrowCompletion(a newly created TypeError object)).
                                return TRY(close_all_open_iterators(vm, vm.throw_completion<TypeError>(ErrorType::ZipIteratorNotEnoughResults)));
                            }
                        }

                        // iii. Return ReturnCompletion(undefined).
                        return IteratorHelper::IterationResult { js_undefined(), true };

                    // iv. Else,
                    case ZipMode::Longest:
                        // i. Assert: mode is "longest".
                        // ii. If openIters is empty, return ReturnCompletion(undefined).
                        if (m_open_iterators.is_empty())
                            return IteratorHelper::IterationResult { js_undefined(), true };

                        // iii. Set iters[i] to null.
                        m_iterators[i] = nullptr;

                        // iv. Set result to padding[i].
                        result = m_padding[i];

                        break;
                    }
                }
            }

            // 4. Append result to results.
            results.append(result.release_value());
        }

        // iv. Set results to finishResults(results).
        auto results_array = m_finish_results->function()(m_realm, results);

        // v. Let completion be Completion(Yield(results)).
        return IteratorHelper::IterationResult { results_array, false };
    }

    ThrowCompletionOr<Value> on_abrupt_completion(VM& vm, Completion const& completion) const
    {
        // vi. If completion is an abrupt completion, then
        //     1. Return ? IteratorCloseAll(openIters, completion).
        return TRY(iterator_close_all(vm, m_open_iterators, completion));
    }

    ReadonlySpan<GC::Ref<IteratorRecord>> open_iterators() const { return m_open_iterators; }
    void set_finish_results(GC::Ref<FinishResults> finish_results) { m_finish_results = finish_results; }

    void append_iterator(GC::Ref<IteratorRecord> iterator)
    {
        m_iterators.append(iterator);
        m_open_iterators.append(iterator);
    }

    void append_padding(Value padding)
    {
        m_padding.append(padding);
    }

private:
    ZipIterator(Realm& realm, ZipMode mode)
        : m_realm(realm)
        , m_mode(mode)
    {
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_realm);
        visitor.visit(m_iterators);
        visitor.visit(m_open_iterators);
        visitor.visit(m_padding);
        visitor.visit(m_finish_results);
    }

    void remove_iterator_from_open_iterators(GC::Ptr<IteratorRecord> iterarator)
    {
        m_open_iterators.remove_first_matching([&](GC::Ref<IteratorRecord> candidate) {
            return candidate == iterarator;
        });
    }

    ThrowCompletionOr<IteratorHelper::IterationResult> close_all_open_iterators(VM& vm, Completion completion) const
    {
        auto close_result = TRY(iterator_close_all(vm, m_open_iterators, completion));
        return IteratorHelper::IterationResult { close_result, true };
    }

    GC::Ref<Realm> m_realm;

    ZipMode m_mode { ZipMode::Shortest };

    Vector<GC::Ptr<IteratorRecord>> m_iterators;
    Vector<GC::Ref<IteratorRecord>> m_open_iterators;

    Vector<Value> m_padding;

    GC::Ptr<FinishResults> m_finish_results;
};

GC_DEFINE_ALLOCATOR(ZipIterator);

// 3 IteratorZip ( iters, mode, padding, finishResults ), https://tc39.es/proposal-joint-iteration/#sec-IteratorZip
static GC::Ref<IteratorHelper> iterator_zip(Realm& realm, GC::Ref<ZipIterator> zip_iterator)
{
    // 1. Let iterCount be the number of elements in iters.
    // 2. Let openIters be a copy of iters.

    // 3. Let closure be a new Abstract Closure with no parameters that captures iters, iterCount, openIters, mode,
    //    padding, and finishResults, and performs the following steps when called:
    auto closure = GC::create_function(realm.heap(), [zip_iterator](VM& vm, IteratorHelper&) -> ThrowCompletionOr<IteratorHelper::IterationResult> {
        return zip_iterator->next(vm);
    });
    auto abrupt_closure = GC::create_function(realm.heap(), [zip_iterator](VM& vm, Completion const& completion) -> ThrowCompletionOr<Value> {
        return zip_iterator->on_abrupt_completion(vm, completion);
    });

    // 4. Let gen be CreateIteratorFromClosure(closure, "Iterator Helper", %IteratorHelperPrototype%, « [[UnderlyingIterators]] »).
    // 5. Set gen.[[UnderlyingIterators]] to openIters.
    // 6. Return gen.
    return IteratorHelper::create(realm, zip_iterator->open_iterators(), closure, abrupt_closure);
}

static ThrowCompletionOr<ZipMode> get_zip_mode(VM& vm, Object const& options)
{
    // 3. Let mode be ? Get(options, "mode").
    auto mode = TRY(options.get(vm.names.mode));

    // 4. If mode is undefined, set mode to "shortest".
    if (mode.is_undefined())
        return ZipMode::Shortest;

    // 5. If mode is not one of "shortest", "longest", or "strict", throw a TypeError exception.
    if (mode.is_string()) {
        auto mode_string = mode.as_string().utf8_string_view();

        if (mode_string == "shortest"sv)
            return ZipMode::Shortest;
        if (mode_string == "longest"sv)
            return ZipMode::Longest;
        if (mode_string == "strict"sv)
            return ZipMode::Strict;
    }

    return vm.throw_completion<TypeError>(ErrorType::OptionIsNotValidValue, mode, vm.names.mode);
}

static ThrowCompletionOr<GC::Ptr<Object>> get_padding_option(VM& vm, Object const& options, ZipMode mode)
{
    // 6. Let paddingOption be undefined.
    GC::Ptr<Object> padding_option;

    // 7. If mode is "longest", then
    if (mode == ZipMode::Longest) {
        // a. Set paddingOption to ? Get(options, "padding").
        auto padding_value = TRY(options.get(vm.names.padding));

        // b. If paddingOption is not undefined and paddingOption is not an Object, throw a TypeError exception.
        if (!padding_value.is_undefined()) {
            if (!padding_value.is_object())
                return vm.throw_completion<TypeError>(ErrorType::OptionIsNotValidValue, padding_value, vm.names.padding);

            padding_option = padding_value.as_object();
        }
    }

    return padding_option;
}

// 1 Iterator.zip ( iterables [ , options ] ), https://tc39.es/proposal-joint-iteration/#sec-iterator.zip
JS_DEFINE_NATIVE_FUNCTION(IteratorConstructor::zip)
{
    auto& realm = *vm.current_realm();

    auto iterables = vm.argument(0);
    auto options_value = vm.argument(1);

    // 1. If iterables is not an Object, throw a TypeError exception.
    if (!iterables.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, iterables);

    // 2. Set options to ? GetOptionsObject(options).
    auto options = TRY(get_options_object(vm, options_value));

    // 3. Let mode be ? Get(options, "mode").
    // 4. If mode is undefined, set mode to "shortest".
    // 5. If mode is not one of "shortest", "longest", or "strict", throw a TypeError exception.
    auto mode = TRY(get_zip_mode(vm, options));

    // 6. Let paddingOption be undefined.
    // 7. If mode is "longest", then
    //     a. Set paddingOption to ? Get(options, "padding").
    //     b. If paddingOption is not undefined and paddingOption is not an Object, throw a TypeError exception.
    auto padding_option = TRY(get_padding_option(vm, options, mode));

    // 8. Let iters be a new empty List.
    // 9. Let padding be a new empty List.
    auto zip_iterator = realm.create<ZipIterator>(realm, mode);

    // 10. Let inputIter be ? GetIterator(iterables, SYNC).
    auto input_iterator = TRY(get_iterator(vm, iterables, IteratorHint::Sync));

    // 11. Let next be NOT-STARTED.
    Optional<Value> next;

    // 12. Repeat, while next is not DONE,
    do {
        // a. Set next to Completion(IteratorStepValue(inputIter)).
        // b. IfAbruptCloseIterators(next, iters).
        next = TRY_OR_CLOSE_ITERATORS(vm, zip_iterator->open_iterators(), iterator_step_value(vm, input_iterator));

        // c. If next is not DONE, then
        if (next.has_value()) {
            // i. Let iter be Completion(GetIteratorFlattenable(next, REJECT-PRIMITIVES)).
            auto iterator = get_iterator_flattenable(vm, *next, PrimitiveHandling::RejectPrimitives);

            // ii. IfAbruptCloseIterators(iter, the list-concatenation of « inputIter » and iters).
            if (iterator.is_error()) {
                // NB: We don't use TRY_OR_CLOSE_ITERATORS above in order to avoid creating a separate vector for the
                //     IteratorCloseAll invocation. IteratorCloseAll would close the list in reverse order, which we
                //     match here.
                auto error = iterator_close_all(vm, zip_iterator->open_iterators(), iterator.release_error());
                return iterator_close(vm, input_iterator, error);
            }

            // iii. Append iter to iters.
            zip_iterator->append_iterator(iterator.release_value());
        }
    } while (next.has_value());

    // 13. Let iterCount be the number of elements in iters.
    auto iterator_count = zip_iterator->open_iterators().size();

    // 14. If mode is "longest", then
    if (mode == ZipMode::Longest) {
        // a. If paddingOption is undefined, then
        if (!padding_option) {
            // i. Perform the following steps iterCount times:
            for (size_t i = 0; i < iterator_count; ++i) {
                // 1. Append undefined to padding.
                zip_iterator->append_padding(js_undefined());
            }
        }
        // b. Else,
        else {
            // i. Let paddingIter be Completion(GetIterator(paddingOption, SYNC)).
            // ii. IfAbruptCloseIterators(paddingIter, iters).
            auto padding_iter = TRY_OR_CLOSE_ITERATORS(vm, zip_iterator->open_iterators(), get_iterator(vm, padding_option, IteratorHint::Sync));

            // iii. Let usingIterator be true.
            auto using_iterator = true;

            // iv. Perform the following steps iterCount times:
            for (size_t i = 0; i < iterator_count; ++i) {
                // 1. If usingIterator is true, then
                if (using_iterator) {
                    // a. Set next to Completion(IteratorStepValue(paddingIter)).
                    // b. IfAbruptCloseIterators(next, iters).
                    next = TRY_OR_CLOSE_ITERATORS(vm, zip_iterator->open_iterators(), iterator_step_value(vm, padding_iter));

                    // c. If next is DONE, then
                    if (!next.has_value()) {
                        // i. Set usingIterator to false.
                        using_iterator = false;
                    }
                    // d. Else,
                    else {
                        // i. Append next to padding.
                        zip_iterator->append_padding(*next);
                    }
                }

                // 2. If usingIterator is false, append undefined to padding.
                if (!using_iterator)
                    zip_iterator->append_padding(js_undefined());
            }

            // v. If usingIterator is true, then
            if (using_iterator) {
                // 1. Let completion be Completion(IteratorClose(paddingIter, NormalCompletion(UNUSED))).
                // 2. IfAbruptCloseIterators(completion, iters).
                TRY_OR_CLOSE_ITERATORS(vm, zip_iterator->open_iterators(), iterator_close(vm, padding_iter, normal_completion(js_undefined())));
            }
        }
    }

    // 15. Let finishResults be a new Abstract Closure with parameters (results) that captures nothing and performs the
    //     following steps when called:
    zip_iterator->set_finish_results(GC::create_function(vm.heap(), [](Realm& realm, ReadonlySpan<Value> results) -> Value {
        // a. Return CreateArrayFromList(results).
        return Array::create_from(realm, results);
    }));

    // 16. Return IteratorZip(iters, mode, padding, finishResults).
    return iterator_zip(realm, zip_iterator);
}

}
