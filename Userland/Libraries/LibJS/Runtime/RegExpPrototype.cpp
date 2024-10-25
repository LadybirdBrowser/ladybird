/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Function.h>
#include <AK/Utf16View.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/ErrorTypes.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/RegExpConstructor.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/RegExpPrototype.h>
#include <LibJS/Runtime/RegExpStringIterator.h>
#include <LibJS/Runtime/StringPrototype.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

JS_DEFINE_ALLOCATOR(RegExpPrototype);

RegExpPrototype::RegExpPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void RegExpPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.test, test, 1, attr);
    define_native_function(realm, vm.names.exec, exec, 1, attr);
    define_native_function(realm, vm.names.compile, compile, 2, attr);

    define_native_function(realm, vm.well_known_symbol_match(), symbol_match, 1, attr);
    define_native_function(realm, vm.well_known_symbol_match_all(), symbol_match_all, 1, attr);
    define_native_function(realm, vm.well_known_symbol_replace(), symbol_replace, 2, attr);
    define_native_function(realm, vm.well_known_symbol_search(), symbol_search, 1, attr);
    define_native_function(realm, vm.well_known_symbol_split(), symbol_split, 2, attr);

    define_native_accessor(realm, vm.names.flags, flags, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.source, source, {}, Attribute::Configurable);

#define __JS_ENUMERATE(FlagName, flagName, flag_name, flag_char) \
    define_native_accessor(realm, vm.names.flagName, flag_name, {}, Attribute::Configurable);
    JS_ENUMERATE_REGEXP_FLAGS
#undef __JS_ENUMERATE
}

// Non-standard abstraction around steps used by multiple prototypes.
static ThrowCompletionOr<void> increment_last_index(VM& vm, Object& regexp_object, Utf16View const& string, bool unicode)
{
    // Let thisIndex be ℝ(? ToLength(? Get(rx, "lastIndex"))).
    auto last_index_value = TRY(regexp_object.get(vm.names.lastIndex));
    auto last_index = TRY(last_index_value.to_length(vm));

    // Let nextIndex be AdvanceStringIndex(S, thisIndex, fullUnicode).
    last_index = advance_string_index(string, last_index, unicode);

    // Perform ? Set(rx, "lastIndex", 𝔽(nextIndex), true).
    TRY(regexp_object.set(vm.names.lastIndex, Value(last_index), Object::ShouldThrowExceptions::Yes));
    return {};
}

// 22.2.7.5 Match Records, https://tc39.es/ecma262/#sec-match-records
struct Match {
    static Match create(regex::Match const& match)
    {
        return { match.global_offset, match.global_offset + match.view.length() };
    }

    size_t start_index { 0 };
    size_t end_index { 0 };
};

// 22.2.7.7 GetMatchIndexPair ( S, match ), https://tc39.es/ecma262/#sec-getmatchindexpair
static Value get_match_index_par(VM& vm, Utf16View const& string, Match const& match)
{
    auto& realm = *vm.current_realm();

    // 1. Assert: match.[[StartIndex]] is an integer value ≥ 0 and ≤ the length of S.
    VERIFY(match.start_index <= string.length_in_code_units());

    // 2. Assert: match.[[EndIndex]] is an integer value ≥ match.[[StartIndex]] and ≤ the length of S.
    VERIFY(match.end_index >= match.start_index);
    VERIFY(match.end_index <= string.length_in_code_units());

    // 3. Return CreateArrayFromList(« match.[[StartIndex]], match.[[EndIndex]] »).
    return Array::create_from(realm, { Value(match.start_index), Value(match.end_index) });
}

// 22.2.7.8 MakeMatchIndicesIndexPairArray ( S, indices, groupNames, hasGroups ), https://tc39.es/ecma262/#sec-makematchindicesindexpairarray
static Value make_match_indices_index_pair_array(VM& vm, Utf16View const& string, Vector<Optional<Match>> const& indices, HashMap<DeprecatedFlyString, Match> const& group_names, bool has_groups)
{
    // Note: This implementation differs from the spec, but has the same behavior.
    //
    // The spec dictates that [[RegExpMatcher]] results should contain one list of capture groups,
    // where each entry holds its group name (if it has one). However, LibRegex stores named capture
    // groups in a separate hash map.
    //
    // The spec further specifies that the group names provided to this abstraction align with the
    // provided indices starting at indices[1], where any entry in indices that does not have a group
    // name is undefined in the group names list. But, the undefined groups names are then just
    // dropped when copying them to the output array.
    //
    // Therefore, this implementation tracks the group names without the assertion that the group
    // names align with the indices. The end result is the same.

    auto& realm = *vm.current_realm();

    // 1. Let n be the number of elements in indices.
    // 2. Assert: n < 2^32-1.
    VERIFY(indices.size() < NumericLimits<u32>::max());

    // 3. Assert: groupNames is a List with n - 1 elements.
    // 4. NOTE: The groupNames List contains elements aligned with the indices List starting at indices[1].

    // 5. Set A to ! ArrayCreate(n).
    auto array = MUST(Array::create(realm, indices.size()));

    // 6. If hasGroups is true, then
    //     a. Let groups be ! ObjectCreate(null).
    // 7. Else,
    //     a. Let groups be undefined.
    auto groups = has_groups ? Object::create(realm, nullptr) : js_undefined();

    // 9. For each integer i such that i ≥ 0 and i < n, do
    for (size_t i = 0; i < indices.size(); ++i) {
        // a. Let matchIndices be indices[i].
        auto const& match_indices = indices[i];

        // b. If matchIndices is not undefined, then
        //     i. Let matchIndicesArray be ! GetMatchIndicesArray(S, matchIndices).
        // c. Else,
        //     i. Let matchIndicesArray be undefined.
        auto match_indices_array = js_undefined();
        if (match_indices.has_value())
            match_indices_array = get_match_index_par(vm, string, *match_indices);

        // d. Perform ! CreateDataPropertyOrThrow(A, ! ToString(i), matchIndicesArray).
        MUST(array->create_data_property_or_throw(i, match_indices_array));
    }

    for (auto const& entry : group_names) {
        auto match_indices_array = get_match_index_par(vm, string, entry.value);

        // e. If i > 0 and groupNames[i - 1] is not undefined, then
        //     i. Assert: groups is not undefined.
        //     ii. Perform ! CreateDataPropertyOrThrow(groups, groupNames[i - 1], matchIndicesArray).
        MUST(groups.as_object().create_data_property_or_throw(entry.key, match_indices_array));
    }

    // 8. Perform ! CreateDataPropertyOrThrow(A, "groups", groups).
    // NOTE: This step must be performed after the above loops in order for groups to be populated.
    MUST(array->create_data_property_or_throw(vm.names.groups, groups));

    // 10. Return A.
    return array;
}

// 22.2.7.2 RegExpBuiltinExec ( R, S ), https://tc39.es/ecma262/#sec-regexpbuiltinexec
// 22.2.7.2 RegExpBuiltInExec ( R, S ), https://github.com/tc39/proposal-regexp-legacy-features#regexpbuiltinexec--r-s-
static ThrowCompletionOr<Value> regexp_builtin_exec(VM& vm, RegExpObject& regexp_object, Utf16String string)
{
    auto& realm = *vm.current_realm();

    // 1. Let length be the length of S.
    // 2. Let lastIndex be ℝ(? ToLength(? Get(R, "lastIndex"))).
    auto last_index_value = TRY(regexp_object.get(vm.names.lastIndex));
    auto last_index = TRY(last_index_value.to_length(vm));

    auto const& regex = regexp_object.regex();

    // 3. Let flags be R.[[OriginalFlags]].
    // 4. If flags contains "g", let global be true; else let global be false.
    bool global = regex.options().has_flag_set(ECMAScriptFlags::Global);
    // 5. If flags contains "y", let sticky be true; else let sticky be false.
    bool sticky = regex.options().has_flag_set(ECMAScriptFlags::Sticky);
    // 6. If flags contains "d", let hasIndices be true, else let hasIndices be false.
    bool has_indices = regexp_object.flags().find('d').has_value();

    // 7. If global is false and sticky is false, set lastIndex to 0.
    if (!global && !sticky)
        last_index = 0;

    // 8. Let matcher be R.[[RegExpMatcher]].

    // 9. If flags contains "u" or flags contains "v", let fullUnicode be true; else let fullUnicode be false.
    bool full_unicode = regex.options().has_flag_set(ECMAScriptFlags::Unicode) || regex.options().has_flag_set(ECMAScriptFlags::UnicodeSets);

    RegexResult result;

    // NOTE: For optimisation purposes, this whole loop is implemented in LibRegex.
    // 10. Let matchSucceeded be false.
    // 11. If fullUnicode is true, let input be StringToCodePoints(S). Otherwise, let input be a List whose elements are the code units that are the elements of S.
    // 12. NOTE: Each element of input is considered to be a character.
    // 13. Repeat, while matchSucceeded is false
    //   a. If lastIndex > length, then
    //       i. If global is true or sticky is true, then
    //           1. Perform ? Set(R, "lastIndex", 0, true).
    //       ii. Return null.
    //   b. Let inputIndex be the index into input of the character that was obtained from element lastIndex of S.
    //   c. Let r be matcher(input, inputIndex).
    //   d. If r is failure, then
    //       i. If sticky is true, then
    //           1. Perform ? Set(R, "lastIndex", 0, true).
    //           2. Return null.
    //       ii. Set lastIndex to AdvanceStringIndex(S, lastIndex, fullUnicode).
    //   e. Else,
    //       i. Assert: r is a State.
    //       ii. Set matchSucceeded to true.

    // 13.b and 13.c
    regex.start_offset = full_unicode ? string.view().code_point_offset_of(last_index) : last_index;
    result = regex.match(string.view());

    // 13.d and 13.a
    if (!result.success) {
        // 13.d.i, 13.a.i
        if (sticky || global)
            TRY(regexp_object.set(vm.names.lastIndex, Value(0), Object::ShouldThrowExceptions::Yes));

        // 13.a.ii, 13.d.i.2
        return js_null();
    }

    auto& match = result.matches[0];
    auto match_index = match.global_offset;

    // 14. Let e be r's endIndex value.
    // https://tc39.es/ecma262/#sec-notation: The endIndex is one plus the index of the last input character matched so far by the pattern.
    auto end_index = match_index + match.view.length();

    // 15. If fullUnicode is true, set e to ! GetStringIndex(S, Input, e).
    if (full_unicode) {
        match_index = string.view().code_unit_offset_of(match.global_offset);
        end_index = string.view().code_unit_offset_of(end_index);
    }

    // 16. If global is true or sticky is true, then
    if (global || sticky) {
        // a. Perform ? Set(R, "lastIndex", 𝔽(e), true).
        TRY(regexp_object.set(vm.names.lastIndex, Value(end_index), Object::ShouldThrowExceptions::Yes));
    }

    // 17. Let n be the number of elements in r's captures List. (This is the same value as 22.2.2.1's NcapturingParens.)
    // 18. Assert: n = R.[[RegExpRecord]].[[CapturingGroupsCount]].
    // 19. Assert: n < 2^32 - 1.
    VERIFY(result.n_named_capture_groups < NumericLimits<u32>::max());

    // 20. Let A be ! ArrayCreate(n + 1).
    auto array = MUST(Array::create(realm, result.n_named_capture_groups + 1));

    // 21. Assert: The mathematical value of A's "length" property is n + 1.

    // 22. Perform ! CreateDataPropertyOrThrow(A, "index", 𝔽(lastIndex)).
    MUST(array->create_data_property_or_throw(vm.names.index, Value(match_index)));

    // 24. Let match be the Match { [[StartIndex]]: lastIndex, [[EndIndex]]: e }.
    auto match_indices = Match::create(match);

    // 25. Let indices be a new empty List.
    Vector<Optional<Match>> indices;
    Vector<Utf16String> captured_values;

    // 26. Let groupNames be a new empty List.
    HashMap<DeprecatedFlyString, Match> group_names;

    // 27. Add match as the last element of indices.
    indices.append(move(match_indices));

    // 28. Let matchedValue be ! GetMatchString(S, match).
    // 29. Perform ! CreateDataPropertyOrThrow(A, "0", matchedValue).
    MUST(array->create_data_property_or_throw(0, PrimitiveString::create(vm, Utf16String::create(match.view.u16_view()))));

    // 30. If R contains any GroupName, then
    //     a. Let groups be OrdinaryObjectCreate(null).
    //     b. Let hasGroups be true.
    // 31. Else,
    //     a. Let groups be undefined.
    //     b. Let hasGroups be false.
    bool has_groups = result.n_named_capture_groups != 0;
    auto groups_object = has_groups ? Object::create(realm, nullptr) : GCPtr<Object> {};

    // 33. For each integer i such that i ≥ 1 and i ≤ n, in ascending order, do
    for (size_t i = 1; i <= result.n_capture_groups; ++i) {
        // a. Let captureI be ith element of r's captures List.
        auto& capture = result.capture_group_matches[0][i];

        Value captured_value;

        // b. If captureI is undefined, then
        if (capture.view.is_null()) {
            // i. Let capturedValue be undefined.
            captured_value = js_undefined();
            // ii. Append undefined to indices.
            indices.append({});
            // iii. Append capture to indices.
            captured_values.append(Utf16String::create());
        }
        // c. Else,
        else {
            // i. Let captureStart be captureI's startIndex.
            // ii. Let captureEnd be captureI's endIndex.
            // iii. If fullUnicode is true, then
            //     1. Set captureStart to ! GetStringIndex(S, Input, captureStart).
            //     2. Set captureEnd to ! GetStringIndex(S, Input, captureEnd).
            // iv. Let capture be the Match { [[StartIndex]]: captureStart, [[EndIndex]: captureEnd }.
            // v. Let capturedValue be ! GetMatchString(S, capture).
            auto capture_as_utf16_string = Utf16String::create(capture.view.u16_view());
            captured_value = PrimitiveString::create(vm, capture_as_utf16_string);
            // vi. Append capture to indices.
            indices.append(Match::create(capture));
            // vii. Append capturedValue to the end of capturedValues.
            captured_values.append(capture_as_utf16_string);
        }

        // d. Perform ! CreateDataPropertyOrThrow(A, ! ToString(𝔽(i)), capturedValue).
        MUST(array->create_data_property_or_throw(i, captured_value));

        // e. If the ith capture of R was defined with a GroupName, then
        if (capture.capture_group_name.has_value()) {
            // i. Let s be the CapturingGroupName of the corresponding RegExpIdentifierName.
            auto group_name = capture.capture_group_name.release_value();

            // ii. Perform ! CreateDataPropertyOrThrow(groups, s, capturedValue).
            MUST(groups_object->create_data_property_or_throw(group_name, captured_value));

            // iii. Append s to groupNames.
            group_names.set(move(group_name), Match::create(capture));
        }
        // f. Else,
        else {
            // i. Append undefined to groupNames.
            // See the note in MakeIndicesArray for why this step is skipped.
        }
    }

    // https://github.com/tc39/proposal-regexp-legacy-features#regexpbuiltinexec--r-s-
    // 5. Let thisRealm be the current Realm Record.
    auto* this_realm = &realm;
    // 6. Let rRealm be the value of R's [[Realm]] internal slot.
    auto* regexp_object_realm = &regexp_object.realm();
    // 7. If SameValue(thisRealm, rRealm) is true, then
    if (this_realm == regexp_object_realm) {
        // i. If the value of R’s [[LegacyFeaturesEnabled]] internal slot is true, then
        if (regexp_object.legacy_features_enabled()) {
            // a. Perform UpdateLegacyRegExpStaticProperties(%RegExp%, S, lastIndex, e, capturedValues).
            update_legacy_regexp_static_properties(realm.intrinsics().regexp_constructor(), string, match_indices.start_index, match_indices.end_index, captured_values);
        }
        // ii. Else,
        else {
            // a. Perform InvalidateLegacyRegExpStaticProperties(%RegExp%).
            invalidate_legacy_regexp_static_properties(realm.intrinsics().regexp_constructor());
        }
    }

    // 32. Perform ! CreateDataPropertyOrThrow(A, "groups", groups).
    // NOTE: This step must be performed after the above loop in order for groups to be populated.
    Value groups = has_groups ? groups_object : js_undefined();
    MUST(array->create_data_property_or_throw(vm.names.groups, groups));

    // 34. If hasIndices is true, then
    if (has_indices) {
        // a. Let indicesArray be MakeMatchIndicesIndexPairArray(S, indices, groupNames, hasGroups).
        auto indices_array = make_match_indices_index_pair_array(vm, string.view(), indices, group_names, has_groups);
        // b. Perform ! CreateDataProperty(A, "indices", indicesArray).
        MUST(array->create_data_property(vm.names.indices, indices_array));
    }

    // 23. Perform ! CreateDataPropertyOrThrow(A, "input", S).
    // NOTE: This step is performed last to allow the string to be moved into the PrimitiveString::create() invocation.
    MUST(array->create_data_property_or_throw(vm.names.input, PrimitiveString::create(vm, move(string))));

    // 35. Return A.
    return array;
}

// 22.2.7.1 RegExpExec ( R, S ), https://tc39.es/ecma262/#sec-regexpexec
ThrowCompletionOr<Value> regexp_exec(VM& vm, Object& regexp_object, Utf16String string)
{
    // 1. Let exec be ? Get(R, "exec").
    auto exec = TRY(regexp_object.get(vm.names.exec));

    // 2. If IsCallable(exec) is true, then
    if (exec.is_function()) {
        // a. Let result be ? Call(exec, R, « S »).
        auto result = TRY(call(vm, exec.as_function(), &regexp_object, PrimitiveString::create(vm, move(string))));

        // b. If Type(result) is neither Object nor Null, throw a TypeError exception.
        if (!result.is_object() && !result.is_null())
            return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOrNull, result.to_string_without_side_effects());

        // c. Return result.
        return result;
    }

    // 3. Perform ? RequireInternalSlot(R, [[RegExpMatcher]]).
    if (!is<RegExpObject>(regexp_object))
        return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "RegExp");

    // 4. Return ? RegExpBuiltinExec(R, S).
    return regexp_builtin_exec(vm, static_cast<RegExpObject&>(regexp_object), move(string));
}

// 22.2.7.3 AdvanceStringIndex ( S, index, unicode ), https://tc39.es/ecma262/#sec-advancestringindex
size_t advance_string_index(Utf16View const& string, size_t index, bool unicode)
{
    // 1. Assert: index ≤ 2^53 - 1.

    // 2. If unicode is false, return index + 1.
    if (!unicode)
        return index + 1;

    // 3. Let length be the length of S.
    // 4. If index + 1 ≥ length, return index + 1.
    if (index + 1 >= string.length_in_code_units())
        return index + 1;

    // 5. Let cp be CodePointAt(S, index).
    auto code_point = code_point_at(string, index);

    // 6. Return index + cp.[[CodeUnitCount]].
    return index + code_point.code_unit_count;
}

// 22.2.6.3 get RegExp.prototype.dotAll, https://tc39.es/ecma262/#sec-get-regexp.prototype.dotAll
// 22.2.6.5 get RegExp.prototype.global, https://tc39.es/ecma262/#sec-get-regexp.prototype.global
// 22.2.6.6 get RegExp.prototype.hasIndices, https://tc39.es/ecma262/#sec-get-regexp.prototype.hasIndices
// 22.2.6.7 get RegExp.prototype.ignoreCase, https://tc39.es/ecma262/#sec-get-regexp.prototype.ignorecase
// 22.2.6.10 get RegExp.prototype.multiline, https://tc39.es/ecma262/#sec-get-regexp.prototype.multiline
// 22.2.6.15 get RegExp.prototype.sticky, https://tc39.es/ecma262/#sec-get-regexp.prototype.sticky
// 22.2.6.18 get RegExp.prototype.unicode, https://tc39.es/ecma262/#sec-get-regexp.prototype.unicode
// 22.2.6.19 get RegExp.prototype.unicodeSets, https://tc39.es/ecma262/#sec-get-regexp.prototype.unicodesets
#define __JS_ENUMERATE(FlagName, flagName, flag_name, flag_char)                           \
    JS_DEFINE_NATIVE_FUNCTION(RegExpPrototype::flag_name)                                  \
    {                                                                                      \
        auto& realm = *vm.current_realm();                                                 \
        /* 1. If Type(R) is not Object, throw a TypeError exception. */                    \
        auto regexp_object = TRY(this_object(vm));                                         \
        /* 2. If R does not have an [[OriginalFlags]] internal slot, then */               \
        if (!is<RegExpObject>(*regexp_object)) {                                           \
            /* a. If SameValue(R, %RegExp.prototype%) is true, return undefined. */        \
            if (same_value(regexp_object, realm.intrinsics().regexp_prototype()))          \
                return js_undefined();                                                     \
            /* b. Otherwise, throw a TypeError exception. */                               \
            return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "RegExp"); \
        }                                                                                  \
        /* 3. Let flags be R.[[OriginalFlags]]. */                                         \
        auto flags = static_cast<RegExpObject&>(*regexp_object).flag_bits();               \
        /* 4. If flags contains codeUnit, return true. */                                  \
        /* 5. Return false. */                                                             \
        return Value(has_flag(flags, RegExpObject::Flags::FlagName));                      \
    }
JS_ENUMERATE_REGEXP_FLAGS
#undef __JS_ENUMERATE

// 22.2.6.2 RegExp.prototype.exec ( string ), https://tc39.es/ecma262/#sec-regexp.prototype.exec
JS_DEFINE_NATIVE_FUNCTION(RegExpPrototype::exec)
{
    // 1. Let R be the this value.
    // 2. Perform ? RequireInternalSlot(R, [[RegExpMatcher]]).
    auto regexp_object = TRY(typed_this_object(vm));

    // 3. Let S be ? ToString(string).
    auto string = TRY(vm.argument(0).to_utf16_string(vm));

    // 4. Return ? RegExpBuiltinExec(R, S).
    return TRY(regexp_builtin_exec(vm, regexp_object, move(string)));
}

// 22.2.6.4 get RegExp.prototype.flags, https://tc39.es/ecma262/#sec-get-regexp.prototype.flags
JS_DEFINE_NATIVE_FUNCTION(RegExpPrototype::flags)
{

    // 1. Let R be the this value.
    // 2. If Type(R) is not Object, throw a TypeError exception.
    auto regexp_object = TRY(this_object(vm));

    // 3. Let result be the empty String.
    StringBuilder builder(8);

    // 4. Let hasIndices be ToBoolean(? Get(R, "hasIndices")).
    // 5. If hasIndices is true, append the code unit 0x0064 (LATIN SMALL LETTER D) as the last code unit of result.
    // 6. Let global be ToBoolean(? Get(R, "global")).
    // 7. If global is true, append the code unit 0x0067 (LATIN SMALL LETTER G) as the last code unit of result.
    // 8. Let ignoreCase be ToBoolean(? Get(R, "ignoreCase")).
    // 9. If ignoreCase is true, append the code unit 0x0069 (LATIN SMALL LETTER I) as the last code unit of result.
    // 10. Let multiline be ToBoolean(? Get(R, "multiline")).
    // 11. If multiline is true, append the code unit 0x006D (LATIN SMALL LETTER M) as the last code unit of result.
    // 12. Let dotAll be ToBoolean(? Get(R, "dotAll")).
    // 13. If dotAll is true, append the code unit 0x0073 (LATIN SMALL LETTER S) as the last code unit of result.
    // 14. Let unicode be ToBoolean(? Get(R, "unicode")).
    // 15. If unicode is true, append the code unit 0x0075 (LATIN SMALL LETTER U) as the last code unit of result.
    // 16. Let unicodeSets be ! ToBoolean(? Get(R, "unicodeSets")).
    // 17. If unicodeSets is true, append the code unit 0x0076 (LATIN SMALL LETTER V) as the last code unit of result.
    // 18. Let sticky be ToBoolean(? Get(R, "sticky")).
    // 19. If sticky is true, append the code unit 0x0079 (LATIN SMALL LETTER Y) as the last code unit of result.
#define __JS_ENUMERATE(FlagName, flagName, flag_name, flag_char)        \
    auto flag_##flag_name = TRY(regexp_object->get(vm.names.flagName)); \
    if (flag_##flag_name.to_boolean())                                  \
        builder.append(#flag_char##sv);
    JS_ENUMERATE_REGEXP_FLAGS
#undef __JS_ENUMERATE

    // 20. Return result.
    return PrimitiveString::create(vm, builder.to_byte_string());
}

// 22.2.6.8 RegExp.prototype [ @@match ] ( string ), https://tc39.es/ecma262/#sec-regexp.prototype-@@match
JS_DEFINE_NATIVE_FUNCTION(RegExpPrototype::symbol_match)
{
    auto& realm = *vm.current_realm();

    // 1. Let rx be the this value.
    // 2. If Type(rx) is not Object, throw a TypeError exception.
    auto regexp_object = TRY(this_object(vm));

    // 3. Let S be ? ToString(string).
    auto string = TRY(vm.argument(0).to_utf16_string(vm));

    // 4. Let flags be ? ToString(? Get(rx, "flags")).
    auto flags_value = TRY(regexp_object->get(vm.names.flags));
    auto flags = TRY(flags_value.to_byte_string(vm));

    // 5. If flags does not contain "g", then
    if (!flags.contains('g')) {
        // a. Return ? RegExpExec(rx, S).
        return TRY(regexp_exec(vm, regexp_object, move(string)));
    }

    // 6. Else,
    // a. If flags contains "u" or flags contains "v", let fullUnicode be true. Otherwise, let fullUnicode be false.
    bool full_unicode = flags.contains('u') || flags.contains('v');

    // b. Perform ? Set(rx, "lastIndex", +0𝔽, true).
    TRY(regexp_object->set(vm.names.lastIndex, Value(0), Object::ShouldThrowExceptions::Yes));

    // c. Let A be ! ArrayCreate(0).
    auto array = MUST(Array::create(realm, 0));

    // d. Let n be 0.
    size_t n = 0;

    // e. Repeat,
    while (true) {
        // i. Let result be ? RegExpExec(rx, S).
        auto result_value = TRY(regexp_exec(vm, regexp_object, string));

        // ii. If result is null, then
        if (result_value.is_null()) {
            // 1. If n = 0, return null.
            if (n == 0)
                return js_null();

            // 2. Return A.
            return array;
        }

        VERIFY(result_value.is_object());
        auto& result = result_value.as_object();

        // iii. Else,

        // 1. Let matchStr be ? ToString(? Get(result, "0")).
        auto match_value = TRY(result.get(0));
        auto match_str = TRY(match_value.to_byte_string(vm));

        // 2. Perform ! CreateDataPropertyOrThrow(A, ! ToString(𝔽(n)), matchStr).
        MUST(array->create_data_property_or_throw(n, PrimitiveString::create(vm, match_str)));

        // 3. If matchStr is the empty String, then
        if (match_str.is_empty()) {
            // Steps 3a-3c are implemented by increment_last_index.
            TRY(increment_last_index(vm, regexp_object, string.view(), full_unicode));
        }

        // 4. Set n to n + 1.
        ++n;
    }
}

// 22.2.6.9 RegExp.prototype [ @@matchAll ] ( string ), https://tc39.es/ecma262/#sec-regexp-prototype-matchall
JS_DEFINE_NATIVE_FUNCTION(RegExpPrototype::symbol_match_all)
{
    auto& realm = *vm.current_realm();

    // 1. Let R be the this value.
    // 2. If Type(R) is not Object, throw a TypeError exception.
    auto regexp_object = TRY(this_object(vm));

    // 3. Let S be ? ToString(string).
    auto string = TRY(vm.argument(0).to_utf16_string(vm));

    // 4. Let C be ? SpeciesConstructor(R, %RegExp%).
    auto* constructor = TRY(species_constructor(vm, regexp_object, realm.intrinsics().regexp_constructor()));

    // 5. Let flags be ? ToString(? Get(R, "flags")).
    auto flags_value = TRY(regexp_object->get(vm.names.flags));
    auto flags = TRY(flags_value.to_byte_string(vm));

    // Steps 9-12 are performed early so that flags can be moved.

    // 9. If flags contains "g", let global be true.
    // 10. Else, let global be false.
    bool global = flags.contains('g');

    // 11. If flags contains "u" or flags contains "v", let fullUnicode be true.
    // 12. Else, let fullUnicode be false.
    bool full_unicode = flags.contains('u') || flags.contains('v');

    // 6. Let matcher be ? Construct(C, « R, flags »).
    auto matcher = TRY(construct(vm, *constructor, regexp_object, PrimitiveString::create(vm, move(flags))));

    // 7. Let lastIndex be ? ToLength(? Get(R, "lastIndex")).
    auto last_index_value = TRY(regexp_object->get(vm.names.lastIndex));
    auto last_index = TRY(last_index_value.to_length(vm));

    // 8. Perform ? Set(matcher, "lastIndex", lastIndex, true).
    TRY(matcher->set(vm.names.lastIndex, Value(last_index), Object::ShouldThrowExceptions::Yes));

    // 13. Return CreateRegExpStringIterator(matcher, S, global, fullUnicode).
    return RegExpStringIterator::create(realm, matcher, move(string), global, full_unicode);
}

// 22.2.6.11 RegExp.prototype [ @@replace ] ( string, replaceValue ), https://tc39.es/ecma262/#sec-regexp.prototype-@@replace
JS_DEFINE_NATIVE_FUNCTION(RegExpPrototype::symbol_replace)
{
    auto string_value = vm.argument(0);
    auto replace_value = vm.argument(1);

    // 1. Let rx be the this value.
    // 2. If Type(rx) is not Object, throw a TypeError exception.
    auto regexp_object = TRY(this_object(vm));

    // 3. Let S be ? ToString(string).
    auto string = TRY(string_value.to_utf16_string(vm));

    // 4. Let lengthS be the number of code unit elements in S.
    // 5. Let functionalReplace be IsCallable(replaceValue).

    // 6. If functionalReplace is false, then
    if (!replace_value.is_function()) {
        // a. Set replaceValue to ? ToString(replaceValue).
        auto replace_string = TRY(replace_value.to_byte_string(vm));
        replace_value = PrimitiveString::create(vm, move(replace_string));
    }

    // 7. Let flags be ? ToString(? Get(rx, "flags")).
    auto flags_value = TRY(regexp_object->get(vm.names.flags));
    auto flags = TRY(flags_value.to_byte_string(vm));

    // 8. If flags contains "g", let global be true. Otherwise, let global be false.
    bool global = flags.contains('g');

    // 9. If global is true, then
    if (global) {
        // a. Perform ? Set(rx, "lastIndex", +0𝔽, true).
        TRY(regexp_object->set(vm.names.lastIndex, Value(0), Object::ShouldThrowExceptions::Yes));
    }

    // 10. Let results be a new empty List.
    MarkedVector<Object*> results(vm.heap());

    // 11. Let done be false.
    // 12. Repeat, while done is false,
    while (true) {
        // a. Let result be ? RegExpExec(rx, S).
        auto result = TRY(regexp_exec(vm, regexp_object, string));

        // b. If result is null, set done to true.
        if (result.is_null())
            break;

        // c. Else,

        // i. Append result to the end of results.
        results.append(&result.as_object());

        // ii. If global is false, set done to true.
        if (!global)
            break;

        // iii. Else,

        // 1. Let matchStr be ? ToString(? Get(result, "0")).
        auto match_value = TRY(result.get(vm, 0));
        auto match_str = TRY(match_value.to_byte_string(vm));

        // 2. If matchStr is the empty String, then
        if (match_str.is_empty()) {
            // b. If flags contains "u" or flags contains "v", let fullUnicode be true. Otherwise, let fullUnicode be false.
            bool full_unicode = flags.contains('u') || flags.contains('v');

            // Steps 2a, 2c-2d are implemented by increment_last_index.
            TRY(increment_last_index(vm, regexp_object, string.view(), full_unicode));
        }
    }

    // 13. Let accumulatedResult be the empty String.
    StringBuilder accumulated_result;

    // 14. Let nextSourcePosition be 0.
    size_t next_source_position = 0;

    // 15. For each element result of results, do
    for (auto& result : results) {
        // a. Let resultLength be ? LengthOfArrayLike(result).
        size_t result_length = TRY(length_of_array_like(vm, *result));

        // b. Let nCaptures be max(resultLength - 1, 0).
        size_t n_captures = result_length == 0 ? 0 : result_length - 1;

        // c. Let matched be ? ToString(? Get(result, "0")).
        auto matched_value = TRY(result->get(0));
        auto matched = TRY(matched_value.to_utf16_string(vm));

        // d. Let matchLength be the length of matched.
        auto matched_length = matched.length_in_code_units();

        // e. Let position be ? ToIntegerOrInfinity(? Get(result, "index")).
        auto position_value = TRY(result->get(vm.names.index));
        double position = TRY(position_value.to_integer_or_infinity(vm));

        // f. Set position to the result of clamping position between 0 and lengthS.
        position = clamp(position, static_cast<double>(0), static_cast<double>(string.length_in_code_units()));

        // g. Let captures be a new empty List.
        MarkedVector<Value> captures(vm.heap());

        // h. Let n be 1.
        // i. Repeat, while n ≤ nCaptures,
        for (size_t n = 1; n <= n_captures; ++n) {
            // i. Let capN be ? Get(result, ! ToString(𝔽(n))).
            auto capture = TRY(result->get(n));

            // ii. If capN is not undefined, then
            if (!capture.is_undefined()) {
                // 1. Set capN to ? ToString(capN).
                capture = PrimitiveString::create(vm, TRY(capture.to_byte_string(vm)));
            }

            // iii. Append capN as the last element of captures.
            captures.append(move(capture));

            // iv. NOTE: When n = 1, the preceding step puts the first element into captures (at index 0). More generally, the nth capture (the characters captured by the nth set of capturing parentheses) is at captures[n - 1].
            // v. Set n to n + 1.
        }

        // j. Let namedCaptures be ? Get(result, "groups").
        auto named_captures = TRY(result->get(vm.names.groups));

        String replacement;

        // k. If functionalReplace is true, then
        if (replace_value.is_function()) {
            // i. Let replacerArgs be the list-concatenation of « matched », captures, and « 𝔽(position), S ».
            MarkedVector<Value> replacer_args(vm.heap());
            replacer_args.append(PrimitiveString::create(vm, move(matched)));
            replacer_args.extend(move(captures));
            replacer_args.append(Value(position));
            replacer_args.append(PrimitiveString::create(vm, string));

            // ii. If namedCaptures is not undefined, then
            if (!named_captures.is_undefined()) {
                // 1. Append namedCaptures as the last element of replacerArgs.
                replacer_args.append(move(named_captures));
            }

            // iii. Let replValue be ? Call(replaceValue, undefined, replacerArgs).
            auto replace_result = TRY(call(vm, replace_value.as_function(), js_undefined(), replacer_args.span()));

            // iv. Let replacement be ? ToString(replValue).
            replacement = TRY(replace_result.to_string(vm));
        }
        // l. Else,
        else {
            /// i. If namedCaptures is not undefined, then
            if (!named_captures.is_undefined()) {
                // 1. Set namedCaptures to ? ToObject(namedCaptures).
                named_captures = TRY(named_captures.to_object(vm));
            }

            // ii. Let replacement be ? GetSubstitution(matched, S, position, captures, namedCaptures, replaceValue).
            replacement = TRY(get_substitution(vm, matched.view(), string.view(), position, captures, named_captures, replace_value));
        }

        // m. If position ≥ nextSourcePosition, then
        if (position >= next_source_position) {
            // i. NOTE: position should not normally move backwards. If it does, it is an indication of an ill-behaving RegExp subclass or use of an access triggered side-effect to change the global flag or other characteristics of rx. In such cases, the corresponding substitution is ignored.

            // ii. Set accumulatedResult to the string-concatenation of accumulatedResult, the substring of S from nextSourcePosition to position, and replacement.
            auto substring = string.substring_view(next_source_position, position - next_source_position);
            accumulated_result.append(substring);
            accumulated_result.append(replacement);

            // iii. Set nextSourcePosition to position + matchLength.
            next_source_position = position + matched_length;
        }
    }

    // 16. If nextSourcePosition ≥ lengthS, return accumulatedResult.
    if (next_source_position >= string.length_in_code_units())
        return PrimitiveString::create(vm, accumulated_result.to_byte_string());

    // 17. Return the string-concatenation of accumulatedResult and the substring of S from nextSourcePosition.
    auto substring = string.substring_view(next_source_position);
    accumulated_result.append(substring);

    return PrimitiveString::create(vm, accumulated_result.to_byte_string());
}

// 22.2.6.12 RegExp.prototype [ @@search ] ( string ), https://tc39.es/ecma262/#sec-regexp.prototype-@@search
JS_DEFINE_NATIVE_FUNCTION(RegExpPrototype::symbol_search)
{
    // 1. Let rx be the this value.
    // 2. If Type(rx) is not Object, throw a TypeError exception.
    auto regexp_object = TRY(this_object(vm));

    // 3. Let S be ? ToString(string).
    auto string = TRY(vm.argument(0).to_utf16_string(vm));

    // 4. Let previousLastIndex be ? Get(rx, "lastIndex").
    auto previous_last_index = TRY(regexp_object->get(vm.names.lastIndex));

    // 5. If SameValue(previousLastIndex, +0𝔽) is false, then
    if (!same_value(previous_last_index, Value(0))) {
        // a. Perform ? Set(rx, "lastIndex", +0𝔽, true).
        TRY(regexp_object->set(vm.names.lastIndex, Value(0), Object::ShouldThrowExceptions::Yes));
    }

    // 6. Let result be ? RegExpExec(rx, S).
    auto result = TRY(regexp_exec(vm, regexp_object, move(string)));

    // 7. Let currentLastIndex be ? Get(rx, "lastIndex").
    auto current_last_index = TRY(regexp_object->get(vm.names.lastIndex));

    // 8. If SameValue(currentLastIndex, previousLastIndex) is false, then
    if (!same_value(current_last_index, previous_last_index)) {
        // a. Perform ? Set(rx, "lastIndex", previousLastIndex, true).
        TRY(regexp_object->set(vm.names.lastIndex, previous_last_index, Object::ShouldThrowExceptions::Yes));
    }

    // 9. If result is null, return -1𝔽.
    if (result.is_null())
        return Value(-1);

    // 10. Return ? Get(result, "index").
    return TRY(result.get(vm, vm.names.index));
}

// 22.2.6.13 get RegExp.prototype.source, https://tc39.es/ecma262/#sec-get-regexp.prototype.source
JS_DEFINE_NATIVE_FUNCTION(RegExpPrototype::source)
{
    auto& realm = *vm.current_realm();

    // 1. Let R be the this value.
    // 2. If Type(R) is not Object, throw a TypeError exception.
    auto regexp_object = TRY(this_object(vm));

    // 3. If R does not have an [[OriginalSource]] internal slot, then
    if (!is<RegExpObject>(*regexp_object)) {
        // a. If SameValue(R, %RegExp.prototype%) is true, return "(?:)".
        if (same_value(regexp_object, realm.intrinsics().regexp_prototype()))
            return PrimitiveString::create(vm, "(?:)"_string);

        // b. Otherwise, throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "RegExp");
    }

    // 4. Assert: R has an [[OriginalFlags]] internal slot.
    // 5. Let src be R.[[OriginalSource]].
    // 6. Let flags be R.[[OriginalFlags]].
    // 7. Return EscapeRegExpPattern(src, flags).
    return PrimitiveString::create(vm, static_cast<RegExpObject&>(*regexp_object).escape_regexp_pattern());
}

// 22.2.6.14 RegExp.prototype [ @@split ] ( string, limit ), https://tc39.es/ecma262/#sec-regexp.prototype-@@split
JS_DEFINE_NATIVE_FUNCTION(RegExpPrototype::symbol_split)
{
    auto& realm = *vm.current_realm();

    // 1. Let rx be the this value.
    // 2. If Type(rx) is not Object, throw a TypeError exception.
    auto regexp_object = TRY(this_object(vm));

    // 3. Let S be ? ToString(string).
    auto string = TRY(vm.argument(0).to_utf16_string(vm));

    // 4. Let C be ? SpeciesConstructor(rx, %RegExp%).
    auto* constructor = TRY(species_constructor(vm, regexp_object, realm.intrinsics().regexp_constructor()));

    // 5. Let flags be ? ToString(? Get(rx, "flags")).
    auto flags_value = TRY(regexp_object->get(vm.names.flags));
    auto flags = TRY(flags_value.to_byte_string(vm));

    // 6. If flags contains "u" or flags contains "v", let unicodeMatching be true.
    // 7. Else, let unicodeMatching be false.
    bool unicode_matching = flags.contains('u') || flags.contains('v');

    // 8. If flags contains "y", let newFlags be flags.
    // 9. Else, let newFlags be the string-concatenation of flags and "y".
    auto new_flags = flags.find('y').has_value() ? move(flags) : ByteString::formatted("{}y", flags);

    // 10. Let splitter be ? Construct(C, « rx, newFlags »).
    auto splitter = TRY(construct(vm, *constructor, regexp_object, PrimitiveString::create(vm, move(new_flags))));

    // 11. Let A be ! ArrayCreate(0).
    auto array = MUST(Array::create(realm, 0));

    // 12. Let lengthA be 0.
    size_t array_length = 0;

    // 13. If limit is undefined, let lim be 2^32 - 1; else let lim be ℝ(? ToUint32(limit)).
    auto limit = NumericLimits<u32>::max();
    if (!vm.argument(1).is_undefined())
        limit = TRY(vm.argument(1).to_u32(vm));

    // 14. If lim is 0, return A.
    if (limit == 0)
        return array;

    // 15. If S is the empty String, then
    if (string.is_empty()) {
        // a. Let z be ? RegExpExec(splitter, S).
        auto result = TRY(regexp_exec(vm, splitter, string));

        // b. If z is not null, return A.
        if (!result.is_null())
            return array;

        // c. Perform ! CreateDataPropertyOrThrow(A, "0", S).
        MUST(array->create_data_property_or_throw(0, PrimitiveString::create(vm, move(string))));

        // d. Return A.
        return array;
    }

    // 16. Let size be the length of S.

    // 17. Let p be 0.
    size_t last_match_end = 0;

    // 18. Let q be p.
    size_t next_search_from = 0;

    // 19. Repeat, while q < size,
    while (next_search_from < string.length_in_code_units()) {
        // a. Perform ? Set(splitter, "lastIndex", 𝔽(q), SplitBehavior::KeepEmpty).
        TRY(splitter->set(vm.names.lastIndex, Value(next_search_from), Object::ShouldThrowExceptions::Yes));

        // b. Let z be ? RegExpExec(splitter, S).
        auto result = TRY(regexp_exec(vm, splitter, string));

        // c. If z is null, set q to AdvanceStringIndex(S, q, unicodeMatching).
        if (result.is_null()) {
            next_search_from = advance_string_index(string.view(), next_search_from, unicode_matching);
            continue;
        }

        // d. Else,

        // i. Let e be ℝ(? ToLength(? Get(splitter, "lastIndex"))).
        auto last_index_value = TRY(splitter->get(vm.names.lastIndex));
        auto last_index = TRY(last_index_value.to_length(vm));

        // ii. Set e to min(e, size).
        last_index = min(last_index, string.length_in_code_units());

        // iii. If e = p, set q to AdvanceStringIndex(S, q, unicodeMatching).
        if (last_index == last_match_end) {
            next_search_from = advance_string_index(string.view(), next_search_from, unicode_matching);
            continue;
        }

        // iv. Else,

        // 1. Let T be the substring of S from p to q.
        auto substring = string.substring_view(last_match_end, next_search_from - last_match_end);

        // 2. Perform ! CreateDataPropertyOrThrow(A, ! ToString(𝔽(lengthA)), T).
        MUST(array->create_data_property_or_throw(array_length, PrimitiveString::create(vm, Utf16String::create(substring))));

        // 3. Set lengthA to lengthA + 1.
        ++array_length;

        // 4. If lengthA = lim, return A.
        if (array_length == limit)
            return array;

        // 5. Set p to e.
        last_match_end = last_index;

        // 6. Let numberOfCaptures be ? LengthOfArrayLike(z).
        auto number_of_captures = TRY(length_of_array_like(vm, result.as_object()));

        // 7. Set numberOfCaptures to max(numberOfCaptures - 1, 0).
        if (number_of_captures > 0)
            --number_of_captures;

        // 8. Let i be 1.
        // 9. Repeat, while i ≤ numberOfCaptures,
        for (size_t i = 1; i <= number_of_captures; ++i) {
            // a. Let nextCapture be ? Get(z, ! ToString(𝔽(i))).
            auto next_capture = TRY(result.get(vm, i));

            // b. Perform ! CreateDataPropertyOrThrow(A, ! ToString(𝔽(lengthA)), nextCapture).
            MUST(array->create_data_property_or_throw(array_length, next_capture));

            // c. Set i to i + 1.

            // d. Set lengthA to lengthA + 1.
            ++array_length;

            // e. If lengthA = lim, return A.
            if (array_length == limit)
                return array;
        }

        // 10. Set q to p.
        next_search_from = last_match_end;
    }

    // 20. Let T be the substring of S from p to size.
    auto substring = string.substring_view(last_match_end);

    // 21. Perform ! CreateDataPropertyOrThrow(A, ! ToString(𝔽(lengthA)), T).
    MUST(array->create_data_property_or_throw(array_length, PrimitiveString::create(vm, Utf16String::create(substring))));

    // 22. Return A.
    return array;
}

// 22.2.6.16 RegExp.prototype.test ( S ), https://tc39.es/ecma262/#sec-regexp.prototype.test
JS_DEFINE_NATIVE_FUNCTION(RegExpPrototype::test)
{
    // 1. Let R be the this value.
    // 2. If Type(R) is not Object, throw a TypeError exception.
    auto regexp_object = TRY(this_object(vm));

    // 3. Let string be ? ToString(S).
    auto string = TRY(vm.argument(0).to_utf16_string(vm));

    // 4. Let match be ? RegExpExec(R, string).
    auto match = TRY(regexp_exec(vm, regexp_object, move(string)));

    // 5. If match is not null, return true; else return false.
    return Value(!match.is_null());
}

// 22.2.6.17 RegExp.prototype.toString ( ), https://tc39.es/ecma262/#sec-regexp.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(RegExpPrototype::to_string)
{
    // 1. Let R be the this value.
    // 2. If Type(R) is not Object, throw a TypeError exception.
    auto regexp_object = TRY(this_object(vm));

    // 3. Let pattern be ? ToString(? Get(R, "source")).
    auto source_attr = TRY(regexp_object->get(vm.names.source));
    auto pattern = TRY(source_attr.to_byte_string(vm));

    // 4. Let flags be ? ToString(? Get(R, "flags")).
    auto flags_attr = TRY(regexp_object->get(vm.names.flags));
    auto flags = TRY(flags_attr.to_byte_string(vm));

    // 5. Let result be the string-concatenation of "/", pattern, "/", and flags.
    // 6. Return result.
    return PrimitiveString::create(vm, ByteString::formatted("/{}/{}", pattern, flags));
}

// B.2.4.1 RegExp.prototype.compile ( pattern, flags ), https://tc39.es/ecma262/#sec-regexp.prototype.compile
// B.2.4.1 RegExp.prototype.compile ( pattern, flags ), https://github.com/tc39/proposal-regexp-legacy-features#regexpprototypecompile--pattern-flags-
JS_DEFINE_NATIVE_FUNCTION(RegExpPrototype::compile)
{
    auto pattern = vm.argument(0);
    auto flags = vm.argument(1);

    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[RegExpMatcher]]).
    auto regexp_object = TRY(typed_this_object(vm));

    // 3. Let thisRealm be the current Realm Record.
    auto* this_realm = vm.current_realm();

    // 4. Let oRealm be the value of O’s [[Realm]] internal slot.
    auto* regexp_object_realm = &regexp_object->realm();

    // 5. If SameValue(thisRealm, oRealm) is false, throw a TypeError exception.
    if (this_realm != regexp_object_realm)
        return vm.throw_completion<TypeError>(ErrorType::RegExpCompileError, "thisRealm and oRealm is not same value");

    // 6. If the value of R’s [[LegacyFeaturesEnabled]] internal slot is false, throw a TypeError exception.
    if (!regexp_object->legacy_features_enabled())
        return vm.throw_completion<TypeError>(ErrorType::RegExpCompileError, "legacy features is not enabled");

    // 7. If Type(pattern) is Object and pattern has a [[RegExpMatcher]] internal slot, then
    if (pattern.is_object() && is<RegExpObject>(pattern.as_object())) {
        // a. If flags is not undefined, throw a TypeError exception.
        if (!flags.is_undefined())
            return vm.throw_completion<TypeError>(ErrorType::NotUndefined, flags.to_string_without_side_effects());

        auto& regexp_pattern = static_cast<RegExpObject&>(pattern.as_object());

        // b. Let P be pattern.[[OriginalSource]].
        pattern = PrimitiveString::create(vm, regexp_pattern.pattern());

        // c. Let F be pattern.[[OriginalFlags]].
        flags = PrimitiveString::create(vm, regexp_pattern.flags());
    }
    // 8. Else,
    //     a. Let P be pattern.
    //     b. Let F be flags.

    // 9. Return ? RegExpInitialize(O, P, F).
    return TRY(regexp_object->regexp_initialize(vm, pattern, flags));
}

}
