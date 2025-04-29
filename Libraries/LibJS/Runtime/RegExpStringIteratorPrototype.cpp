/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/RegExpPrototype.h>
#include <LibJS/Runtime/RegExpStringIteratorPrototype.h>
#include <LibJS/Runtime/Utf16String.h>

namespace JS {

GC_DEFINE_ALLOCATOR(RegExpStringIteratorPrototype);

RegExpStringIteratorPrototype::RegExpStringIteratorPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().iterator_prototype())
{
}

void RegExpStringIteratorPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);
    auto& vm = this->vm();

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.next, next, 0, attr);

    // 22.2.9.2.2 %RegExpStringIteratorPrototype% [ @@toStringTag ], https://tc39.es/ecma262/#sec-%regexpstringiteratorprototype%-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "RegExp String Iterator"_string), Attribute::Configurable);
}

// 22.2.9.2.1 %RegExpStringIteratorPrototype%.next ( ), https://tc39.es/ecma262/#sec-%regexpstringiteratorprototype%.next
JS_DEFINE_NATIVE_FUNCTION(RegExpStringIteratorPrototype::next)
{
    // 1. Let O be the this value.
    // 2. If O is not an Object, throw a TypeError exception.
    // 3. If O does not have all of the internal slots of a RegExp String Iterator Object Instance (see 22.2.9.3), throw a TypeError exception.
    auto iterator = TRY(typed_this_value(vm));

    // 4. If O.[[Done]] is true, then
    if (iterator->done()) {
        // a. Return CreateIteratorResultObject(undefined, true).
        return create_iterator_result_object(vm, js_undefined(), true);
    }

    // 5. Let R be O.[[IteratingRegExp]].
    auto& regexp = iterator->regexp_object();

    // 6. Let S be O.[[IteratedString]].
    auto const& string = iterator->string();

    // 7. Let global be O.[[Global]].
    auto global = iterator->global();

    // 8. Let fullUnicode be O.[[Unicode]].
    auto full_unicode = iterator->unicode();

    // 9. Let match be ? RegExpExec(R, S).
    auto match = TRY(regexp_exec(vm, regexp, string));

    // 10. If match is null, then
    if (match.is_null()) {
        // a. Set O.[[Done]] to true.
        iterator->set_done();

        // b. Return CreateIteratorResultObject(undefined, true).
        return create_iterator_result_object(vm, js_undefined(), true);
    }

    // 11. If global is false, then
    if (!global) {
        // a. Set O.[[Done]] to true.
        iterator->set_done();

        // b. Return CreateIteratorResultObject(match, false).
        return create_iterator_result_object(vm, match, false);
    }

    // 12. Let matchStr be ? ToString(? Get(match, "0")).
    auto match_string = TRY(TRY(match.get(vm, 0)).to_utf16_string(vm));

    // 13. If matchStr is the empty String, then
    if (match_string.is_empty()) {
        // a. Let thisIndex be ℝ(? ToLength(? Get(R, "lastIndex"))).
        auto this_index = TRY(TRY(regexp.get(vm.names.lastIndex)).to_length(vm));

        // b. Let nextIndex be AdvanceStringIndex(S, thisIndex, fullUnicode).
        auto next_index = advance_string_index(string.view(), this_index, full_unicode);

        // c. Perform ? Set(R, "lastIndex", 𝔽(nextIndex), true).
        TRY(regexp.set(vm.names.lastIndex, Value { next_index }, Object::ShouldThrowExceptions::Yes));
    }

    // 14. Return CreateIteratorResultObject(match, false).
    return create_iterator_result_object(vm, match, false);
}

}
