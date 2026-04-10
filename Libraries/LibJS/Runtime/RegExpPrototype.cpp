/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Utf16String.h>
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

GC_DEFINE_ALLOCATOR(RegExpPrototype);

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
    define_native_function(realm, vm.names.exec, exec, 1, attr, Bytecode::Builtin::RegExpPrototypeExec);
    define_native_function(realm, vm.names.compile, compile, 2, attr);

    define_native_function(realm, vm.well_known_symbol_match(), symbol_match, 1, attr);
    define_native_function(realm, vm.well_known_symbol_match_all(), symbol_match_all, 1, attr);
    define_native_function(realm, vm.well_known_symbol_replace(), symbol_replace, 2, attr, Bytecode::Builtin::RegExpPrototypeReplace);
    define_native_function(realm, vm.well_known_symbol_search(), symbol_search, 1, attr);
    define_native_function(realm, vm.well_known_symbol_split(), symbol_split, 2, attr, Bytecode::Builtin::RegExpPrototypeSplit);

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
    static Bytecode::StaticPropertyLookupCache cache;
    auto last_index_value = TRY(regexp_object.get(vm.names.lastIndex, cache));
    auto last_index = TRY(last_index_value.to_length(vm));

    // Let nextIndex be AdvanceStringIndex(S, thisIndex, fullUnicode).
    last_index = advance_string_index(string, last_index, unicode);

    // Perform ? Set(rx, "lastIndex", 𝔽(nextIndex), true).
    static Bytecode::StaticPropertyLookupCache cache2;
    TRY(regexp_object.set(vm.names.lastIndex, Value(last_index), cache2));
    return {};
}

// FIXME: Add an eviction policy to bound the size of this cache.
static HashMap<String, NonnullOwnPtr<regex::ECMAScriptRegex>> s_regex_cache;

static regex::ECMAScriptRegex const* get_or_compile_regex(RegExpObject& regexp_object)
{
    // Fast path: check the inline cache on the RegExpObject.
    if (auto* cached = regexp_object.cached_regex())
        return cached;

    auto const& pattern = regexp_object.pattern();
    auto flag_bits = regexp_object.flag_bits();

    // Build a cache key from pattern + flag bits.
    StringBuilder key_builder;
    key_builder.append('/');
    key_builder.append(pattern.utf16_view());
    key_builder.append('/');
    key_builder.append_code_point(static_cast<u8>(flag_bits));
    auto cache_key = key_builder.to_string_without_validation();

    if (auto it = s_regex_cache.find(cache_key); it != s_regex_cache.end()) {
        auto* ptr = it->value.ptr();
        regexp_object.set_cached_regex(ptr);
        return ptr;
    }

    bool unicode = has_flag(flag_bits, RegExpObject::Flags::Unicode);
    bool unicode_sets = has_flag(flag_bits, RegExpObject::Flags::UnicodeSets);

    // Parse the pattern from UTF-16 source to UTF-8 with escape normalization.
    auto parsed_pattern = parse_regex_pattern(pattern.utf16_view(), unicode, unicode_sets);
    if (parsed_pattern.is_error())
        return nullptr;

    regex::ECMAScriptCompileFlags flags {};
    flags.global = has_flag(flag_bits, RegExpObject::Flags::Global);
    flags.ignore_case = has_flag(flag_bits, RegExpObject::Flags::IgnoreCase);
    flags.multiline = has_flag(flag_bits, RegExpObject::Flags::Multiline);
    flags.dot_all = has_flag(flag_bits, RegExpObject::Flags::DotAll);
    flags.unicode = unicode;
    flags.unicode_sets = unicode_sets;
    flags.sticky = has_flag(flag_bits, RegExpObject::Flags::Sticky);
    flags.has_indices = has_flag(flag_bits, RegExpObject::Flags::HasIndices);

    auto compiled = regex::ECMAScriptRegex::compile(parsed_pattern.release_value(), flags);
    if (compiled.is_error())
        return nullptr;

    auto owned = make<regex::ECMAScriptRegex>(compiled.release_value());
    auto* ptr = owned.ptr();
    s_regex_cache.set(cache_key, move(owned));
    regexp_object.set_cached_regex(ptr);
    return ptr;
}

struct ExecWithLastIndexResult {
    regex::MatchResult result;
    size_t effective_last_index;
};

static ExecWithLastIndexResult exec_with_unicode_last_index_retry(regex::ECMAScriptRegex const& compiled_regex, Utf16View const& utf16_view, size_t last_index, bool unicode_mode, bool sticky)
{
    auto exec_at = [&](size_t index) {
        return ExecWithLastIndexResult {
            .result = compiled_regex.exec(utf16_view, index),
            .effective_last_index = index,
        };
    };

    if (!unicode_mode || last_index == 0 || last_index >= utf16_view.length_in_code_units())
        return exec_at(last_index);

    auto current = utf16_view.code_unit_at(last_index);
    auto previous = utf16_view.code_unit_at(last_index - 1);
    if (!(current >= 0xDC00 && current <= 0xDFFF
            && previous >= 0xD800 && previous <= 0xDBFF))
        return exec_at(last_index);

    if (!sticky && compiled_regex.is_single_non_bmp_literal())
        return exec_at(last_index);

    // NB: V8/SpiderMonkey first try the code point that starts at the
    // surrogate pair boundary, but zero-width patterns can still match at the
    // original low-surrogate index when that earlier retry fails. Consuming
    // retries must still be rejected so /u and /v regexes never split the
    // surrogate pair.
    auto snapped_result = exec_at(last_index - 1);
    if (snapped_result.result != regex::MatchResult::NoMatch)
        return snapped_result;

    auto retried_result = exec_at(last_index);
    if (retried_result.result != regex::MatchResult::Match)
        return retried_result;

    auto match_start = compiled_regex.capture_slot(0);
    auto match_end = compiled_regex.capture_slot(1);
    if (match_start >= 0 && match_end >= 0
        && static_cast<size_t>(match_start) == last_index
        && static_cast<size_t>(match_end) == last_index) {
        return retried_result;
    }

    return ExecWithLastIndexResult {
        .result = regex::MatchResult::NoMatch,
        .effective_last_index = last_index,
    };
}

// 22.2.7.2 RegExpBuiltinExec ( R, S ), https://tc39.es/ecma262/#sec-regexpbuiltinexec
// 22.2.7.2 RegExpBuiltInExec ( R, S ), https://github.com/tc39/proposal-regexp-legacy-features#regexpbuiltinexec--r-s-
static ThrowCompletionOr<Value> regexp_builtin_exec(VM& vm, RegExpObject& regexp_object, GC::Ref<PrimitiveString> string)
{
    auto& realm = *vm.current_realm();

    static Bytecode::StaticPropertyLookupCache cache;
    auto last_index_value = TRY(regexp_object.get(vm.names.lastIndex, cache));
    auto last_index = TRY(last_index_value.to_length(vm));

    auto flag_bits = regexp_object.flag_bits();

    bool global = has_flag(flag_bits, RegExpObject::Flags::Global);
    bool sticky = has_flag(flag_bits, RegExpObject::Flags::Sticky);
    bool has_indices = has_flag(flag_bits, RegExpObject::Flags::HasIndices);
    if (!global && !sticky)
        last_index = 0;

    auto utf16_view = string->utf16_string_view();

    if (last_index > string->length_in_utf16_code_units()) {
        if (sticky || global) {
            static Bytecode::StaticPropertyLookupCache cache2;
            TRY(regexp_object.set(vm.names.lastIndex, Value(0), cache2));
        }
        return js_null();
    }

    auto* compiled_regex = get_or_compile_regex(regexp_object);
    if (!compiled_regex)
        return js_null();

    bool unicode_mode = has_flag(flag_bits, RegExpObject::Flags::Unicode)
        || has_flag(flag_bits, RegExpObject::Flags::UnicodeSets);
    auto exec_result = exec_with_unicode_last_index_retry(*compiled_regex, utf16_view, last_index, unicode_mode, sticky);
    if (exec_result.result == regex::MatchResult::LimitExceeded)
        return vm.throw_completion<InternalError>(ErrorType::RegExpBacktrackLimitExceeded);
    bool matched = exec_result.result == regex::MatchResult::Match;

    // For sticky mode, the match must start at exactly lastIndex.
    if (matched && sticky) {
        auto match_start = compiled_regex->capture_slot(0);
        if (match_start < 0 || static_cast<size_t>(match_start) != exec_result.effective_last_index)
            matched = false;
    }

    if (!matched) {
        if (sticky || global) {
            static Bytecode::StaticPropertyLookupCache cache2;
            TRY(regexp_object.set(vm.names.lastIndex, Value(0), cache2));
        }
        return js_null();
    }

    // Group 0 is the full match -- read directly from internal capture buffer.
    auto match_index = static_cast<size_t>(compiled_regex->capture_slot(0));
    auto end_index = static_cast<size_t>(compiled_regex->capture_slot(1));

    // In Unicode mode, match_index and end_index are already in code unit indices from the VM.
    // Update lastIndex.
    if (global || sticky) {
        static Bytecode::StaticPropertyLookupCache cache3;
        TRY(regexp_object.set(vm.names.lastIndex, Value(end_index), cache3));
    }

    auto n_capture_groups = compiled_regex->capture_count();
    auto& named_groups = compiled_regex->named_groups();

    auto array = MUST(Array::create(realm, n_capture_groups + 1));
    array->unsafe_set_shape(realm.intrinsics().regexp_builtin_exec_array_shape());

    // "index" property.
    array->put_direct(realm.intrinsics().regexp_builtin_exec_array_index_offset(), Value(match_index));

    // "input" property.
    array->put_direct(realm.intrinsics().regexp_builtin_exec_array_input_offset(), string);

    // Element 0: the full match substring.
    array->indexed_put(0, PrimitiveString::create(vm, *string, match_index, end_index - match_index));

    bool has_groups = !named_groups.is_empty();
    auto groups = has_groups ? Object::create(realm, nullptr) : js_undefined();

    // "groups" property.
    array->put_direct(realm.intrinsics().regexp_builtin_exec_array_groups_offset(), groups);

    // Track which group names have been matched (non-undefined) to handle duplicate names.
    HashTable<Utf16FlyString> matched_group_names;

    auto total_groups = compiled_regex->total_groups();

    for (unsigned int i = 1; i <= n_capture_groups; ++i) {
        Value captured_value;

        int cap_start = (i < total_groups) ? compiled_regex->capture_slot(i * 2) : -1;
        int cap_end = (i < total_groups) ? compiled_regex->capture_slot(i * 2 + 1) : -1;

        if (cap_start >= 0 && cap_end >= 0) {
            captured_value = PrimitiveString::create(vm, *string, static_cast<size_t>(cap_start), static_cast<size_t>(cap_end - cap_start));
        } else {
            captured_value = js_undefined();
        }

        array->indexed_put(i, captured_value);

        // Named groups: find by linear scan (typically very few named groups).
        for (auto const& ng : named_groups) {
            if (ng.index == i) {
                auto group_name = Utf16FlyString::from_utf8(ng.name);
                if (matched_group_names.contains(group_name)) {
                    // Name already matched with a non-undefined value; skip.
                    break;
                }
                if (!captured_value.is_undefined())
                    matched_group_names.set(group_name);
                MUST(groups.as_object().create_data_property_or_throw(group_name, captured_value));
                break;
            }
        }
    }

    // Ensure named groups are enumerated in source order.
    if (has_groups) {
        auto original_groups = groups;
        groups = Object::create(realm, nullptr);

        for (auto const& ng : named_groups) {
            auto group_name = Utf16FlyString::from_utf8(ng.name);
            auto value = original_groups.as_object().get_without_side_effects(group_name);
            MUST(groups.as_object().create_data_property_or_throw(group_name, value));
        }

        static Bytecode::StaticPropertyLookupCache cache4;
        MUST(array->set(vm.names.groups, groups, cache4));
    }

    // Legacy RegExp static properties (lazy -- defer $1-$9 string creation).
    bool needs_legacy = regexp_object.legacy_features_enabled() && &realm == &regexp_object.realm();
    if (needs_legacy) {
        auto cap_count = min(static_cast<unsigned int>(9), n_capture_groups);
        int cap_starts[9];
        int cap_ends[9];
        for (unsigned int g = 0; g < cap_count; ++g) {
            auto gi = g + 1;
            cap_starts[g] = (gi < total_groups) ? compiled_regex->capture_slot(gi * 2) : -1;
            cap_ends[g] = (gi < total_groups) ? compiled_regex->capture_slot(gi * 2 + 1) : -1;
        }
        update_legacy_regexp_static_properties_lazy(realm.intrinsics().regexp_constructor(), string, match_index, end_index, cap_count, cap_starts, cap_ends);
    } else if (&realm == &regexp_object.realm()) {
        invalidate_legacy_regexp_static_properties(realm.intrinsics().regexp_constructor());
    }

    // hasIndices ("d" flag).
    if (has_indices) {
        auto indices_array = MUST(Array::create(realm, 0));
        // Index 0: full match
        {
            auto pair = MUST(Array::create(realm, 2));
            pair->indexed_put(0, Value(match_index));
            pair->indexed_put(1, Value(end_index));
            indices_array->indexed_put(0, pair);
        }
        for (unsigned int i = 1; i <= n_capture_groups; ++i) {
            int idx_start = (i < total_groups) ? compiled_regex->capture_slot(i * 2) : -1;
            int idx_end = (i < total_groups) ? compiled_regex->capture_slot(i * 2 + 1) : -1;
            if (idx_start >= 0 && idx_end >= 0) {
                auto pair = MUST(Array::create(realm, 2));
                pair->indexed_put(0, Value(static_cast<size_t>(idx_start)));
                pair->indexed_put(1, Value(static_cast<size_t>(idx_end)));
                indices_array->indexed_put(i, pair);
            } else {
                indices_array->indexed_put(i, js_undefined());
            }
        }

        auto indices_groups = has_groups ? Object::create(realm, nullptr) : js_undefined();
        if (has_groups) {
            HashTable<Utf16FlyString> matched_index_group_names;
            for (auto const& ng : named_groups) {
                auto group_name = Utf16FlyString::from_utf8(ng.name);
                if (matched_index_group_names.contains(group_name))
                    continue;
                unsigned int group_idx = ng.index;
                int gi_start = (group_idx < total_groups) ? compiled_regex->capture_slot(group_idx * 2) : -1;
                int gi_end = (group_idx < total_groups) ? compiled_regex->capture_slot(group_idx * 2 + 1) : -1;
                if (gi_start >= 0 && gi_end >= 0) {
                    matched_index_group_names.set(group_name);
                    auto pair = MUST(Array::create(realm, 2));
                    pair->indexed_put(0, Value(static_cast<size_t>(gi_start)));
                    pair->indexed_put(1, Value(static_cast<size_t>(gi_end)));
                    MUST(indices_groups.as_object().create_data_property_or_throw(group_name, pair));
                } else {
                    MUST(indices_groups.as_object().create_data_property_or_throw(group_name, js_undefined()));
                }
            }
        }

        MUST(indices_array->create_data_property_or_throw(vm.names.groups, indices_groups));
        MUST(array->create_data_property_or_throw(vm.names.indices, indices_array));
    }

    return array;
}

// 22.2.7.1 RegExpExec ( R, S ), https://tc39.es/ecma262/#sec-regexpexec
ThrowCompletionOr<Value> regexp_exec(VM& vm, Object& regexp_object, GC::Ref<PrimitiveString> string)
{
    // 1. Let exec be ? Get(R, "exec").
    static Bytecode::StaticPropertyLookupCache cache;
    auto exec = TRY(regexp_object.get(vm.names.exec, cache));

    auto* typed_regexp_object = as_if<RegExpObject>(regexp_object);

    // 2. If IsCallable(exec) is true, then
    if (auto exec_function = exec.as_if<FunctionObject>()) {
        if (typed_regexp_object && exec_function->builtin() == Bytecode::Builtin::RegExpPrototypeExec)
            return regexp_builtin_exec(vm, *typed_regexp_object, string);

        // a. Let result be ? Call(exec, R, « S »).
        auto result = TRY(call(vm, exec_function, &regexp_object, string));

        // b. If Type(result) is neither Object nor Null, throw a TypeError exception.
        if (!result.is_object() && !result.is_null())
            return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOrNull, result);

        // c. Return result.
        return result;
    }

    // 3. Perform ? RequireInternalSlot(R, [[RegExpMatcher]]).
    if (!typed_regexp_object)
        return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "RegExp");

    // 4. Return ? RegExpBuiltinExec(R, S).
    return regexp_builtin_exec(vm, *typed_regexp_object, string);
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
    auto string = TRY(vm.argument(0).to_primitive_string(vm));

    // 4. Return ? RegExpBuiltinExec(R, S).
    return TRY(regexp_builtin_exec(vm, regexp_object, string));
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
#define __JS_ENUMERATE(FlagName, flagName, flag_name, flag_char)                   \
    {                                                                              \
        static Bytecode::StaticPropertyLookupCache cache;                          \
        auto flag_##flag_name = TRY(regexp_object->get(vm.names.flagName, cache)); \
        if (flag_##flag_name.to_boolean())                                         \
            builder.append(#flag_char##sv);                                        \
    }
    JS_ENUMERATE_REGEXP_FLAGS
#undef __JS_ENUMERATE

    // 20. Return result.
    return PrimitiveString::create(vm, builder.to_string_without_validation());
}

// 22.2.6.8 RegExp.prototype [ @@match ] ( string ), https://tc39.es/ecma262/#sec-regexp.prototype-@@match
JS_DEFINE_NATIVE_FUNCTION(RegExpPrototype::symbol_match)
{
    auto& realm = *vm.current_realm();

    // 1. Let rx be the this value.
    // 2. If Type(rx) is not Object, throw a TypeError exception.
    auto regexp_object = TRY(this_object(vm));

    // 3. Let S be ? ToString(string).
    auto string = TRY(vm.argument(0).to_primitive_string(vm));

    // 4. Let flags be ? ToString(? Get(rx, "flags")).
    static Bytecode::StaticPropertyLookupCache cache;
    auto flags_value = TRY(regexp_object->get(vm.names.flags, cache));
    auto flags = TRY(flags_value.to_string(vm));

    // 5. If flags does not contain "g", then
    if (!flags.contains('g')) {
        // a. Return ? RegExpExec(rx, S).
        return TRY(regexp_exec(vm, regexp_object, string));
    }

    // 6. Else,
    // a. If flags contains "u" or flags contains "v", let fullUnicode be true. Otherwise, let fullUnicode be false.
    bool full_unicode = flags.contains('u') || flags.contains('v');

    // b. Perform ? Set(rx, "lastIndex", +0𝔽, true).
    static Bytecode::StaticPropertyLookupCache cache2;
    TRY(regexp_object->set(vm.names.lastIndex, Value(0), cache2));

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
        auto match_str = TRY(match_value.to_string(vm));

        // 2. Perform ! CreateDataPropertyOrThrow(A, ! ToString(𝔽(n)), matchStr).
        array->indexed_put(n, PrimitiveString::create(vm, match_str));

        // 3. If matchStr is the empty String, then
        if (match_str.is_empty()) {
            // Steps 3a-3c are implemented by increment_last_index.
            TRY(increment_last_index(vm, regexp_object, string->utf16_string_view(), full_unicode));
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
    auto string = TRY(vm.argument(0).to_primitive_string(vm));

    // 4. Let C be ? SpeciesConstructor(R, %RegExp%).
    auto* constructor = TRY(species_constructor(vm, regexp_object, realm.intrinsics().regexp_constructor()));

    // 5. Let flags be ? ToString(? Get(R, "flags")).
    static Bytecode::StaticPropertyLookupCache cache;
    auto flags_value = TRY(regexp_object->get(vm.names.flags, cache));
    auto flags = TRY(flags_value.to_string(vm));

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
    static Bytecode::StaticPropertyLookupCache cache2;
    auto last_index_value = TRY(regexp_object->get(vm.names.lastIndex, cache2));
    auto last_index = TRY(last_index_value.to_length(vm));

    // 8. Perform ? Set(matcher, "lastIndex", lastIndex, true).
    static Bytecode::StaticPropertyLookupCache cache3;
    TRY(matcher->set(vm.names.lastIndex, Value(last_index), cache3));

    // 13. Return CreateRegExpStringIterator(matcher, S, global, fullUnicode).
    return RegExpStringIterator::create(realm, matcher, string, global, full_unicode);
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
    auto string = TRY(string_value.to_primitive_string(vm));

    return symbol_replace_impl(vm, *regexp_object, string, replace_value);
}

ThrowCompletionOr<Value> RegExpPrototype::symbol_replace_impl(VM& vm, Object& regexp_object, GC::Ref<PrimitiveString> string, Value replace_value)
{
    // OPTIMIZATION: Fast path for str.replace(regexp, simple_string).
    // When the replacement is a string without $ substitution patterns,
    // we can do the entire replace in C++ without creating any JS objects.
    if (!replace_value.is_function()) {
        auto* typed_regexp = as_if<RegExpObject>(regexp_object);
        // Only use the fast path for unmodified RegExp objects:
        // not a subclass, exec/global/unicode/flags not overridden.
        auto& realm = *vm.current_realm();
        bool exec_is_builtin = false;
        if (typed_regexp) {
            static Bytecode::StaticPropertyLookupCache exec_cache;
            auto exec_val = TRY(regexp_object.get(vm.names.exec, exec_cache));
            if (auto exec_fn = exec_val.as_if<FunctionObject>())
                exec_is_builtin = exec_fn->builtin() == Bytecode::Builtin::RegExpPrototypeExec;
        }
        // Also check that lastIndex is a plain writable number (no valueOf side
        // effects, no non-writable throw). RegExpObject stores lastIndex as a fast
        // property, so we can cheaply check via storage_get.
        bool lastindex_ok = false;
        if (typed_regexp) {
            auto li_and_attrs = typed_regexp->storage_get(vm.names.lastIndex);
            if (li_and_attrs.has_value() && li_and_attrs->value.is_number()
                && li_and_attrs->attributes.is_writable()) {
                lastindex_ok = true;
            }
        }
        auto* regexp_prototype = typed_regexp ? realm.intrinsics().regexp_prototype().ptr() : nullptr;
        if (typed_regexp
            && exec_is_builtin
            && lastindex_ok
            && static_cast<Object const&>(regexp_object).prototype() == regexp_prototype
            && !regexp_object.storage_has(vm.names.global)
            && !regexp_object.storage_has(vm.names.unicode)
            && !regexp_object.storage_has(vm.names.flags)) {
            auto replace_string = TRY(replace_value.to_string(vm));
            bool has_dollar = replace_string.contains('$');

            if (!has_dollar) {
                auto flag_bits = typed_regexp->flag_bits();
                bool is_global = has_flag(flag_bits, RegExpObject::Flags::Global);
                bool is_sticky = has_flag(flag_bits, RegExpObject::Flags::Sticky);
                bool is_unicode = has_flag(flag_bits, RegExpObject::Flags::Unicode);
                bool is_unicode_sets = has_flag(flag_bits, RegExpObject::Flags::UnicodeSets);
                bool full_unicode = is_unicode || is_unicode_sets;

                // Per spec, for global patterns, Get(rx, "unicode") is required
                // (step 9). This Get may have side effects that invalidate our
                // fast path (e.g. redefining exec). Do the Get and re-check exec.
                bool fast_path_valid = true;
                if (is_global) {
                    static Bytecode::StaticPropertyLookupCache unicode_cache;
                    auto unicode_val = TRY(regexp_object.get(vm.names.unicode, unicode_cache));
                    full_unicode = unicode_val.to_boolean();
                    // Re-verify exec is still the builtin after potential side effects.
                    static Bytecode::StaticPropertyLookupCache exec_recheck;
                    auto exec_val2 = TRY(regexp_object.get(vm.names.exec, exec_recheck));
                    auto exec_fn2 = exec_val2.as_if<FunctionObject>();
                    if (!exec_fn2 || exec_fn2->builtin() != Bytecode::Builtin::RegExpPrototypeExec)
                        fast_path_valid = false;
                }

                auto* compiled_regex = fast_path_valid ? get_or_compile_regex(*typed_regexp) : nullptr;
                if (compiled_regex) {
                    auto utf16_view = string->utf16_string_view();
                    auto length_s = utf16_view.length_in_code_units();

                    size_t last_index = 0;
                    if (is_global || is_sticky) {
                        static Bytecode::StaticPropertyLookupCache li_cache;
                        auto li_value = TRY(typed_regexp->get(vm.names.lastIndex, li_cache));
                        last_index = TRY(li_value.to_length(vm));
                    }
                    if (is_global) {
                        TRY(typed_regexp->set(vm.names.lastIndex, Value(0), Object::ShouldThrowExceptions::Yes));
                        last_index = 0;
                    }

                    auto n_capture_groups = compiled_regex->capture_count();
                    auto total_groups = compiled_regex->total_groups();

                    bool need_legacy = typed_regexp->legacy_features_enabled()
                        && &realm == &typed_regexp->realm();

                    StringBuilder accumulated_result;
                    size_t next_source_position = 0;
                    bool had_match = false;
                    size_t last_match_start = 0;
                    size_t last_match_end = 0;

                    // OPTIMIZATION: For global, non-sticky patterns, use batch find_all
                    // to find all matches in a single Rust call.
                    if (is_global && !is_sticky && !full_unicode) {
                        auto num_matches = compiled_regex->find_all(utf16_view, last_index);
                        if (num_matches < 0)
                            return vm.throw_completion<InternalError>(ErrorType::RegExpBacktrackLimitExceeded);
                        if (num_matches > 0) {
                            had_match = true;
                            auto [last_s, last_e] = compiled_regex->find_all_match(num_matches - 1);
                            last_match_start = last_s;
                            last_match_end = last_e;

                            for (int i = 0; i < num_matches; ++i) {
                                auto [match_start, match_end] = compiled_regex->find_all_match(i);
                                if (static_cast<size_t>(match_start) >= next_source_position) {
                                    accumulated_result.append(utf16_view.substring_view(next_source_position, match_start - next_source_position));
                                    accumulated_result.append(replace_string);
                                    next_source_position = match_end;
                                }
                            }
                        }
                    } else {
                        // Loop finding matches and building the result string.
                        while (true) {
                            if (last_index > length_s) {
                                if (is_sticky || is_global) {
                                    static Bytecode::StaticPropertyLookupCache li_cache2;
                                    TRY(typed_regexp->set(vm.names.lastIndex, Value(0), li_cache2));
                                }
                                break;
                            }

                            auto exec_result = exec_with_unicode_last_index_retry(*compiled_regex, utf16_view, last_index, full_unicode, is_sticky);
                            if (exec_result.result == regex::MatchResult::LimitExceeded)
                                return vm.throw_completion<InternalError>(ErrorType::RegExpBacktrackLimitExceeded);
                            bool matched = exec_result.result == regex::MatchResult::Match;

                            // For sticky, match must start at exactly lastIndex.
                            if (matched && is_sticky && static_cast<size_t>(compiled_regex->capture_slot(0)) != exec_result.effective_last_index)
                                matched = false;

                            if (!matched) {
                                if (is_sticky || is_global) {
                                    static Bytecode::StaticPropertyLookupCache li_cache2;
                                    TRY(typed_regexp->set(vm.names.lastIndex, Value(0), li_cache2));
                                }
                                break;
                            }

                            auto match_start = static_cast<size_t>(compiled_regex->capture_slot(0));
                            auto match_end = static_cast<size_t>(compiled_regex->capture_slot(1));
                            auto match_length = match_end - match_start;
                            had_match = true;
                            last_match_start = match_start;
                            last_match_end = match_end;

                            // For sticky (non-global), update lastIndex on each match.
                            // For global, lastIndex is always reset to 0 after the loop,
                            // so skip intermediate updates.
                            if (is_sticky && !is_global) {
                                static Bytecode::StaticPropertyLookupCache li_cache3;
                                TRY(typed_regexp->set(vm.names.lastIndex, Value(match_end), li_cache3));
                            }

                            // Append the part of the string before this match + the replacement.
                            if (match_start >= next_source_position) {
                                accumulated_result.append(utf16_view.substring_view(next_source_position, match_start - next_source_position));
                                accumulated_result.append(replace_string);
                                next_source_position = match_start + match_length;
                            }

                            if (!is_global)
                                break;

                            // Handle empty match advancement.
                            if (match_length == 0) {
                                if (full_unicode)
                                    last_index = advance_string_index(utf16_view, match_end, true);
                                else
                                    last_index = match_end + 1;
                            } else {
                                last_index = match_end;
                            }
                        }
                    } // end else (non-batch path)

                    // Update legacy RegExp static properties once, with the last match.
                    // For string replacements (no function callback), only the final
                    // state matters since JS can't observe intermediate updates.
                    if (need_legacy && had_match) {
                        // For global replace, the internal buffer was overwritten by the
                        // final failed search. Re-exec at the last match position to
                        // populate captures. For non-global, the buffer is still valid.
                        if (is_global) {
                            auto re_exec_result = compiled_regex->exec(utf16_view, last_match_start);
                            if (re_exec_result == regex::MatchResult::LimitExceeded)
                                return vm.throw_completion<InternalError>(ErrorType::RegExpBacktrackLimitExceeded);
                        }
                        auto cap_count = min(static_cast<unsigned int>(9), n_capture_groups);
                        int cap_starts[9];
                        int cap_ends[9];
                        for (unsigned int g = 0; g < cap_count; ++g) {
                            auto gi = g + 1;
                            cap_starts[g] = (gi < total_groups) ? compiled_regex->capture_slot(gi * 2) : -1;
                            cap_ends[g] = (gi < total_groups) ? compiled_regex->capture_slot(gi * 2 + 1) : -1;
                        }
                        update_legacy_regexp_static_properties_lazy(realm.intrinsics().regexp_constructor(), string, last_match_start, last_match_end, cap_count, cap_starts, cap_ends);
                    }

                    // Fast path: if no matches were found, return the original string.
                    if (!had_match)
                        return string;

                    // Append the trailing portion of the string.
                    if (next_source_position < length_s)
                        accumulated_result.append(utf16_view.substring_view(next_source_position));

                    return PrimitiveString::create(vm, accumulated_result.to_string_without_validation());
                }
            }
        }
    }

    // 4. Let lengthS be the number of code unit elements in S.
    // 5. Let functionalReplace be IsCallable(replaceValue).

    // 6. If functionalReplace is false, then
    if (!replace_value.is_function()) {
        // a. Set replaceValue to ? ToString(replaceValue).
        auto replace_string = TRY(replace_value.to_string(vm));
        replace_value = PrimitiveString::create(vm, move(replace_string));
    }

    // 7. Let flags be ? ToString(? Get(rx, "flags")).
    static Bytecode::StaticPropertyLookupCache cache;
    auto flags_value = TRY(regexp_object.get(vm.names.flags, cache));
    auto flags = TRY(flags_value.to_string(vm));

    // 8. If flags contains "g", let global be true. Otherwise, let global be false.
    bool global = flags.contains('g');

    // 9. If global is true, then
    if (global) {
        // a. Perform ? Set(rx, "lastIndex", +0𝔽, true).
        static Bytecode::StaticPropertyLookupCache cache2;
        TRY(regexp_object.set(vm.names.lastIndex, Value(0), cache2));
    }

    // 10. Let results be a new empty List.
    GC::RootVector<Object*> results(vm.heap());

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
        auto match_str = TRY(match_value.to_string(vm));

        // 2. If matchStr is the empty String, then
        if (match_str.is_empty()) {
            // b. If flags contains "u" or flags contains "v", let fullUnicode be true. Otherwise, let fullUnicode be false.
            bool full_unicode = flags.contains('u') || flags.contains('v');

            // Steps 2a, 2c-2d are implemented by increment_last_index.
            TRY(increment_last_index(vm, regexp_object, string->utf16_string_view(), full_unicode));
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
        auto matched = TRY(matched_value.to_primitive_string(vm));

        // d. Let matchLength be the length of matched.
        auto matched_length = matched->length_in_utf16_code_units();

        // e. Let position be ? ToIntegerOrInfinity(? Get(result, "index")).
        static Bytecode::StaticPropertyLookupCache cache2;
        auto position_value = TRY(result->get(vm.names.index, cache2));
        double position = TRY(position_value.to_integer_or_infinity(vm));

        // f. Set position to the result of clamping position between 0 and lengthS.
        position = clamp(position, static_cast<double>(0), static_cast<double>(string->length_in_utf16_code_units()));

        // g. Let captures be a new empty List.
        GC::RootVector<Value> captures(vm.heap());

        // h. Let n be 1.
        // i. Repeat, while n ≤ nCaptures,
        for (size_t n = 1; n <= n_captures; ++n) {
            // i. Let capN be ? Get(result, ! ToString(𝔽(n))).
            auto capture = TRY(result->get(n));

            // ii. If capN is not undefined, then
            if (!capture.is_undefined()) {
                // 1. Set capN to ? ToString(capN).
                capture = PrimitiveString::create(vm, TRY(capture.to_string(vm)));
            }

            // iii. Append capN as the last element of captures.
            captures.append(move(capture));

            // iv. NOTE: When n = 1, the preceding step puts the first element into captures (at index 0). More generally, the nth capture (the characters captured by the nth set of capturing parentheses) is at captures[n - 1].
            // v. Set n to n + 1.
        }

        // j. Let namedCaptures be ? Get(result, "groups").
        static Bytecode::StaticPropertyLookupCache cache3;
        auto named_captures = TRY(result->get(vm.names.groups, cache3));

        String replacement;

        // k. If functionalReplace is true, then
        if (replace_value.is_function()) {
            // i. Let replacerArgs be the list-concatenation of « matched », captures, and « 𝔽(position), S ».
            GC::RootVector<Value> replacer_args(vm.heap());
            replacer_args.append(matched);
            replacer_args.extend(move(captures));
            replacer_args.append(Value(position));
            replacer_args.append(string);

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
            replacement = TRY(get_substitution(vm, matched->utf16_string_view(), string->utf16_string_view(), position, captures, named_captures, replace_value));
        }

        // m. If position ≥ nextSourcePosition, then
        if (position >= next_source_position) {
            // i. NOTE: position should not normally move backwards. If it does, it is an indication of an ill-behaving RegExp subclass or use of an access triggered side-effect to change the global flag or other characteristics of rx. In such cases, the corresponding substitution is ignored.

            // ii. Set accumulatedResult to the string-concatenation of accumulatedResult, the substring of S from nextSourcePosition to position, and replacement.
            auto substring = string->utf16_string_view().substring_view(next_source_position, position - next_source_position);
            accumulated_result.append(substring);
            accumulated_result.append(replacement);

            // iii. Set nextSourcePosition to position + matchLength.
            next_source_position = position + matched_length;
        }
    }

    // 16. If nextSourcePosition ≥ lengthS, return accumulatedResult.
    if (next_source_position >= string->length_in_utf16_code_units())
        return PrimitiveString::create(vm, accumulated_result.to_string_without_validation());

    // 17. Return the string-concatenation of accumulatedResult and the substring of S from nextSourcePosition.
    auto substring = string->utf16_string_view().substring_view(next_source_position);
    accumulated_result.append(substring);

    return PrimitiveString::create(vm, accumulated_result.to_string_without_validation());
}

// 22.2.6.12 RegExp.prototype [ @@search ] ( string ), https://tc39.es/ecma262/#sec-regexp.prototype-@@search
JS_DEFINE_NATIVE_FUNCTION(RegExpPrototype::symbol_search)
{
    // 1. Let rx be the this value.
    // 2. If Type(rx) is not Object, throw a TypeError exception.
    auto regexp_object = TRY(this_object(vm));

    // 3. Let S be ? ToString(string).
    auto string = TRY(vm.argument(0).to_primitive_string(vm));

    // 4. Let previousLastIndex be ? Get(rx, "lastIndex").
    static Bytecode::StaticPropertyLookupCache cache;
    auto previous_last_index = TRY(regexp_object->get(vm.names.lastIndex, cache));

    // 5. If SameValue(previousLastIndex, +0𝔽) is false, then
    if (!same_value(previous_last_index, Value(0))) {
        // a. Perform ? Set(rx, "lastIndex", +0𝔽, true).
        static Bytecode::StaticPropertyLookupCache cache2;
        TRY(regexp_object->set(vm.names.lastIndex, Value(0), cache2));
    }

    // 6. Let result be ? RegExpExec(rx, S).
    auto result = TRY(regexp_exec(vm, regexp_object, string));

    // 7. Let currentLastIndex be ? Get(rx, "lastIndex").
    static Bytecode::StaticPropertyLookupCache cache2;
    auto current_last_index = TRY(regexp_object->get(vm.names.lastIndex, cache2));

    // 8. If SameValue(currentLastIndex, previousLastIndex) is false, then
    if (!same_value(current_last_index, previous_last_index)) {
        // a. Perform ? Set(rx, "lastIndex", previousLastIndex, true).
        static Bytecode::StaticPropertyLookupCache cache3;
        TRY(regexp_object->set(vm.names.lastIndex, previous_last_index, cache3));
    }

    // 9. If result is null, return -1𝔽.
    if (result.is_null())
        return Value(-1);

    // 10. Return ? Get(result, "index").
    static Bytecode::StaticPropertyLookupCache cache3;
    return TRY(result.get(vm, vm.names.index, cache3));
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
    // 1. Let rx be the this value.
    // 2. If Type(rx) is not Object, throw a TypeError exception.
    auto regexp_object = TRY(this_object(vm));

    // 3. Let S be ? ToString(string).
    auto string = TRY(vm.argument(0).to_primitive_string(vm));

    return symbol_split_impl(vm, *regexp_object, string, vm.argument(1));
}

ThrowCompletionOr<Value> RegExpPrototype::symbol_split_impl(VM& vm, Object& regexp_object, GC::Ref<PrimitiveString> string, Value limit_value)
{
    auto& realm = *vm.current_realm();

    // OPTIMIZATION: Fast path for split with regex.
    // When we have an unmodified RegExp, bypass the spec's SpeciesConstructor/Construct
    // overhead and call the regex directly with explicit start positions.
    {
        auto* typed_regexp = as_if<RegExpObject>(regexp_object);
        bool exec_is_builtin = false;
        if (typed_regexp) {
            static Bytecode::StaticPropertyLookupCache exec_cache;
            auto exec_val = TRY(regexp_object.get(vm.names.exec, exec_cache));
            if (auto exec_fn = exec_val.as_if<FunctionObject>())
                exec_is_builtin = exec_fn->builtin() == Bytecode::Builtin::RegExpPrototypeExec;
        }
        if (typed_regexp
            && exec_is_builtin
            && static_cast<Object const&>(regexp_object).prototype() == realm.intrinsics().regexp_prototype()
            && !regexp_object.storage_has(vm.names.flags)
            && !regexp_object.storage_has(vm.names.constructor)
            && !regexp_object.storage_has(vm.well_known_symbol_match())
            && realm.intrinsics().regexp_prototype()->storage_has(vm.names.flags)
            && (limit_value.is_undefined() || limit_value.is_number())) {

            auto* compiled_regex = get_or_compile_regex(*typed_regexp);
            if (compiled_regex) {
                auto flag_bits = typed_regexp->flag_bits();
                bool is_unicode = has_flag(flag_bits, RegExpObject::Flags::Unicode);
                bool is_unicode_sets = has_flag(flag_bits, RegExpObject::Flags::UnicodeSets);
                bool unicode_matching = is_unicode || is_unicode_sets;

                auto limit = NumericLimits<u32>::max();
                if (!limit_value.is_undefined())
                    limit = TRY(limit_value.to_u32(vm));

                auto array = MUST(Array::create(realm, 0));

                if (limit == 0)
                    return array;

                auto utf16_view = string->utf16_string_view();
                auto size = utf16_view.length_in_code_units();

                // Empty string case.
                if (size == 0) {
                    auto empty_result = compiled_regex->exec(utf16_view, 0);
                    if (empty_result == regex::MatchResult::LimitExceeded)
                        return vm.throw_completion<InternalError>(ErrorType::RegExpBacktrackLimitExceeded);
                    if (empty_result != regex::MatchResult::Match)
                        array->indexed_put(0, string);
                    return array;
                }

                size_t array_length = 0;
                size_t last_match_end = 0;
                size_t next_search_from = 0;
                auto n_capture_groups = compiled_regex->capture_count();
                auto total_groups = compiled_regex->total_groups();

                bool need_legacy = typed_regexp->legacy_features_enabled()
                    && &realm == &typed_regexp->realm();

                while (next_search_from < size) {
                    // The spec's split algorithm uses sticky semantics: match must
                    // start at exactly next_search_from. We do a forward search and
                    // then handle the case where the match starts later.
                    auto split_exec_result = compiled_regex->exec(utf16_view, next_search_from);
                    if (split_exec_result == regex::MatchResult::LimitExceeded)
                        return vm.throw_completion<InternalError>(ErrorType::RegExpBacktrackLimitExceeded);
                    bool matched = split_exec_result == regex::MatchResult::Match;

                    if (!matched)
                        break;

                    auto match_start = static_cast<size_t>(compiled_regex->capture_slot(0));
                    auto match_end = static_cast<size_t>(compiled_regex->capture_slot(1));
                    auto last_index = min(match_end, size);

                    // If the match starts at or past the end of string, it's not
                    // a valid split point (spec's while q < size would have exited).
                    if (match_start >= size)
                        break;

                    // If the match doesn't start at next_search_from, skip to
                    // where it does start.
                    if (match_start > next_search_from)
                        next_search_from = match_start;

                    // If match is zero-width at same position as last split, advance.
                    if (last_index == last_match_end) {
                        next_search_from = advance_string_index(utf16_view, next_search_from, unicode_matching);
                        continue;
                    }

                    // Add substring before this match.
                    array->indexed_put(array_length, PrimitiveString::create(vm, *string, last_match_end, next_search_from - last_match_end));
                    ++array_length;
                    if (array_length == limit)
                        return array;

                    last_match_end = last_index;

                    // Update legacy properties lazily.
                    if (need_legacy) {
                        auto cap_count = min(static_cast<unsigned int>(9), n_capture_groups);
                        int cap_starts[9];
                        int cap_ends[9];
                        for (unsigned int g = 0; g < cap_count; ++g) {
                            auto gi = g + 1;
                            cap_starts[g] = (gi < total_groups) ? compiled_regex->capture_slot(gi * 2) : -1;
                            cap_ends[g] = (gi < total_groups) ? compiled_regex->capture_slot(gi * 2 + 1) : -1;
                        }
                        update_legacy_regexp_static_properties_lazy(realm.intrinsics().regexp_constructor(),
                            string, match_start, match_end, cap_count, cap_starts, cap_ends);
                    }

                    // Add captures.
                    for (unsigned int i = 1; i <= n_capture_groups; ++i) {
                        int cap_start = (i < total_groups) ? compiled_regex->capture_slot(i * 2) : -1;
                        int cap_end = (i < total_groups) ? compiled_regex->capture_slot(i * 2 + 1) : -1;

                        if (cap_start >= 0 && cap_end >= 0) {
                            array->indexed_put(array_length, PrimitiveString::create(vm, *string, static_cast<size_t>(cap_start), static_cast<size_t>(cap_end - cap_start)));
                        } else {
                            array->indexed_put(array_length, js_undefined());
                        }
                        ++array_length;
                        if (array_length == limit)
                            return array;
                    }

                    next_search_from = last_match_end;
                }

                // Add trailing substring.
                array->indexed_put(array_length, PrimitiveString::create(vm, *string, last_match_end, size - last_match_end));

                return array;
            }
        }
    }

    // 4. Let C be ? SpeciesConstructor(rx, %RegExp%).
    auto* constructor = TRY(species_constructor(vm, regexp_object, realm.intrinsics().regexp_constructor()));

    // 5. Let flags be ? ToString(? Get(rx, "flags")).
    static Bytecode::StaticPropertyLookupCache cache;
    auto flags_value = TRY(regexp_object.get(vm.names.flags, cache));
    auto flags = TRY(flags_value.to_string(vm));

    // 6. If flags contains "u" or flags contains "v", let unicodeMatching be true.
    // 7. Else, let unicodeMatching be false.
    bool unicode_matching = flags.contains('u') || flags.contains('v');

    // 8. If flags contains "y", let newFlags be flags.
    // 9. Else, let newFlags be the string-concatenation of flags and "y".
    auto new_flags = flags.bytes_as_string_view().find('y').has_value() ? move(flags) : MUST(String::formatted("{}y", flags));

    // 10. Let splitter be ? Construct(C, « rx, newFlags »).
    auto splitter = TRY(construct(vm, *constructor, &regexp_object, PrimitiveString::create(vm, move(new_flags))));

    // 11. Let A be ! ArrayCreate(0).
    auto array = MUST(Array::create(realm, 0));

    // 12. Let lengthA be 0.
    size_t array_length = 0;

    // 13. If limit is undefined, let lim be 2^32 - 1; else let lim be ℝ(? ToUint32(limit)).
    auto limit = NumericLimits<u32>::max();
    if (!limit_value.is_undefined())
        limit = TRY(limit_value.to_u32(vm));

    // 14. If lim is 0, return A.
    if (limit == 0)
        return array;

    // 15. If S is the empty String, then
    if (string->is_empty()) {
        // a. Let z be ? RegExpExec(splitter, S).
        auto result = TRY(regexp_exec(vm, splitter, string));

        // b. If z is not null, return A.
        if (!result.is_null())
            return array;

        // c. Perform ! CreateDataPropertyOrThrow(A, "0", S).
        array->indexed_put(0, string);

        // d. Return A.
        return array;
    }

    // 16. Let size be the length of S.

    // 17. Let p be 0.
    size_t last_match_end = 0;

    // 18. Let q be p.
    size_t next_search_from = 0;

    // 19. Repeat, while q < size,
    while (next_search_from < string->length_in_utf16_code_units()) {
        // a. Perform ? Set(splitter, "lastIndex", 𝔽(q), SplitBehavior::KeepEmpty).
        static Bytecode::StaticPropertyLookupCache cache2;
        TRY(splitter->set(vm.names.lastIndex, Value(next_search_from), cache2));

        // b. Let z be ? RegExpExec(splitter, S).
        auto result = TRY(regexp_exec(vm, splitter, string));

        // c. If z is null, set q to AdvanceStringIndex(S, q, unicodeMatching).
        if (result.is_null()) {
            next_search_from = advance_string_index(string->utf16_string_view(), next_search_from, unicode_matching);
            continue;
        }

        // d. Else,

        // i. Let e be ℝ(? ToLength(? Get(splitter, "lastIndex"))).
        static Bytecode::StaticPropertyLookupCache cache3;
        auto last_index_value = TRY(splitter->get(vm.names.lastIndex, cache3));
        auto last_index = TRY(last_index_value.to_length(vm));

        // ii. Set e to min(e, size).
        last_index = min(last_index, string->length_in_utf16_code_units());

        // iii. If e = p, set q to AdvanceStringIndex(S, q, unicodeMatching).
        if (last_index == last_match_end) {
            next_search_from = advance_string_index(string->utf16_string_view(), next_search_from, unicode_matching);
            continue;
        }

        // iv. Else,

        // 1. Let T be the substring of S from p to q.
        // 2. Perform ! CreateDataPropertyOrThrow(A, ! ToString(𝔽(lengthA)), T).
        array->indexed_put(array_length, PrimitiveString::create(vm, *string, last_match_end, next_search_from - last_match_end));

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
            array->indexed_put(array_length, next_capture);

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
    // 21. Perform ! CreateDataPropertyOrThrow(A, ! ToString(𝔽(lengthA)), T).
    array->indexed_put(array_length, PrimitiveString::create(vm, *string, last_match_end, string->length_in_utf16_code_units() - last_match_end));

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
    auto string = TRY(vm.argument(0).to_primitive_string(vm));

    // OPTIMIZATION: Fast path for test() on non-global, non-sticky RegExp objects.
    // Use the regex test() directly, avoiding result Array creation.
    {
        auto* typed_regexp = as_if<RegExpObject>(*regexp_object);
        auto& realm = *vm.current_realm();
        bool exec_is_builtin = false;
        if (typed_regexp) {
            static Bytecode::StaticPropertyLookupCache exec_cache;
            auto exec_val = TRY(regexp_object->get(vm.names.exec, exec_cache));
            if (auto exec_fn = exec_val.as_if<FunctionObject>())
                exec_is_builtin = exec_fn->builtin() == Bytecode::Builtin::RegExpPrototypeExec;
        }
        if (typed_regexp
            && exec_is_builtin
            && static_cast<Object const&>(*regexp_object).prototype() == realm.intrinsics().regexp_prototype()) {

            auto flag_bits = typed_regexp->flag_bits();
            bool global = has_flag(flag_bits, RegExpObject::Flags::Global);
            bool sticky = has_flag(flag_bits, RegExpObject::Flags::Sticky);

            // Only use fast path when we don't need to update lastIndex.
            if (!global && !sticky) {
                auto* compiled_regex = get_or_compile_regex(*typed_regexp);
                if (compiled_regex) {
                    auto utf16_view = string->utf16_string_view();

                    if (!typed_regexp->legacy_features_enabled()) {
                        // Fastest path: just test, no captures or legacy props.
                        auto test_result = compiled_regex->test(utf16_view, 0);
                        if (test_result == regex::MatchResult::LimitExceeded)
                            return vm.throw_completion<InternalError>(ErrorType::RegExpBacktrackLimitExceeded);
                        return Value(test_result == regex::MatchResult::Match);
                    }

                    // Fast path with legacy property updates: use exec to
                    // get captures, then set legacy props lazily.
                    auto test_exec_result = compiled_regex->exec(utf16_view, 0);
                    if (test_exec_result == regex::MatchResult::LimitExceeded)
                        return vm.throw_completion<InternalError>(ErrorType::RegExpBacktrackLimitExceeded);
                    bool matched = test_exec_result == regex::MatchResult::Match;
                    if (matched) {
                        auto n_capture_groups = compiled_regex->capture_count();
                        auto total_groups = compiled_regex->total_groups();
                        auto match_start = static_cast<size_t>(compiled_regex->capture_slot(0));
                        auto match_end = static_cast<size_t>(compiled_regex->capture_slot(1));
                        auto cap_count = min(static_cast<unsigned int>(9), n_capture_groups);
                        int cap_starts[9];
                        int cap_ends[9];
                        for (unsigned int g = 0; g < cap_count; ++g) {
                            auto gi = g + 1;
                            cap_starts[g] = (gi < total_groups) ? compiled_regex->capture_slot(gi * 2) : -1;
                            cap_ends[g] = (gi < total_groups) ? compiled_regex->capture_slot(gi * 2 + 1) : -1;
                        }
                        update_legacy_regexp_static_properties_lazy(realm.intrinsics().regexp_constructor(),
                            string, match_start, match_end, cap_count, cap_starts, cap_ends);
                    } else {
                        invalidate_legacy_regexp_static_properties(realm.intrinsics().regexp_constructor());
                    }
                    return Value(matched);
                }
            }
        }
    }

    // 4. Let match be ? RegExpExec(R, string).
    auto match = TRY(regexp_exec(vm, *regexp_object, string));

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
    static Bytecode::StaticPropertyLookupCache cache;
    auto source_attr = TRY(regexp_object->get(vm.names.source, cache));
    auto pattern = TRY(source_attr.to_string(vm));

    // 4. Let flags be ? ToString(? Get(R, "flags")).
    static Bytecode::StaticPropertyLookupCache cache2;
    auto flags_attr = TRY(regexp_object->get(vm.names.flags, cache2));
    auto flags = TRY(flags_attr.to_string(vm));

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
    if (auto regexp_pattern = pattern.as_if<RegExpObject>()) {
        // a. If flags is not undefined, throw a TypeError exception.
        if (!flags.is_undefined())
            return vm.throw_completion<TypeError>(ErrorType::NotUndefined, flags);

        // b. Let P be pattern.[[OriginalSource]].
        pattern = PrimitiveString::create(vm, regexp_pattern->pattern());

        // c. Let F be pattern.[[OriginalFlags]].
        flags = PrimitiveString::create(vm, regexp_pattern->flags());
    }
    // 8. Else,
    //     a. Let P be pattern.
    //     b. Let F be flags.

    // 9. Return ? RegExpInitialize(O, P, F).
    return TRY(regexp_object->regexp_initialize(vm, pattern, flags));
}

}
