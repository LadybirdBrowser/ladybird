/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/RegExpConstructor.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

JS_DEFINE_ALLOCATOR(RegExpConstructor);

RegExpConstructor::RegExpConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.RegExp.as_string(), realm.intrinsics().function_prototype())
{
}

void RegExpConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 22.2.5.1 RegExp.prototype, https://tc39.es/ecma262/#sec-regexp.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().regexp_prototype(), 0);

    define_native_accessor(realm, vm.well_known_symbol_species(), symbol_species_getter, {}, Attribute::Configurable);

    define_direct_property(vm.names.length, Value(2), Attribute::Configurable);

    // Additional properties of the RegExp constructor, https://github.com/tc39/proposal-regexp-legacy-features#additional-properties-of-the-regexp-constructor
    define_native_accessor(realm, vm.names.input, input_getter, input_setter, Attribute::Configurable);
    define_native_accessor(realm, vm.names.inputAlias, input_alias_getter, input_alias_setter, Attribute::Configurable);
    define_native_accessor(realm, vm.names.lastMatch, last_match_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.lastMatchAlias, last_match_alias_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.lastParen, last_paren_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.lastParenAlias, last_paren_alias_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.leftContext, left_context_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.leftContextAlias, left_context_alias_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.rightContext, right_context_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.rightContextAlias, right_context_alias_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.$1, group_1_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.$2, group_2_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.$3, group_3_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.$4, group_4_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.$5, group_5_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.$6, group_6_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.$7, group_7_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.$8, group_8_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.$9, group_9_getter, {}, Attribute::Configurable);
}

// 22.2.4.1 RegExp ( pattern, flags ), https://tc39.es/ecma262/#sec-regexp-pattern-flags
ThrowCompletionOr<Value> RegExpConstructor::call()
{
    auto& vm = this->vm();

    auto pattern = vm.argument(0);
    auto flags = vm.argument(1);

    // 1. Let patternIsRegExp be ? IsRegExp(pattern).
    bool pattern_is_regexp = TRY(pattern.is_regexp(vm));

    // 2. If NewTarget is undefined, then
    // a. Let newTarget be the active function object.
    auto& new_target = *this;

    // b. If patternIsRegExp is true and flags is undefined, then
    if (pattern_is_regexp && flags.is_undefined()) {
        // i. Let patternConstructor be ? Get(pattern, "constructor").
        auto pattern_constructor = TRY(pattern.as_object().get(vm.names.constructor));

        // ii. If SameValue(newTarget, patternConstructor) is true, return pattern.
        if (same_value(&new_target, pattern_constructor))
            return pattern;
    }

    return TRY(construct(new_target));
}

// 22.2.4.1 RegExp ( pattern, flags ), https://tc39.es/ecma262/#sec-regexp-pattern-flags
ThrowCompletionOr<NonnullGCPtr<Object>> RegExpConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto pattern = vm.argument(0);
    auto flags = vm.argument(1);

    // 1. Let patternIsRegExp be ? IsRegExp(pattern).
    bool pattern_is_regexp = TRY(pattern.is_regexp(vm));

    // NOTE: Step 2 is handled in call() above.
    // 3. Else, let newTarget be NewTarget.

    Value pattern_value;
    Value flags_value;

    // 4. If pattern is an Object and pattern has a [[RegExpMatcher]] internal slot, then
    if (pattern.is_object() && is<RegExpObject>(pattern.as_object())) {
        // a. Let P be pattern.[[OriginalSource]].
        auto& regexp_pattern = static_cast<RegExpObject&>(pattern.as_object());
        pattern_value = PrimitiveString::create(vm, regexp_pattern.pattern());

        // b. If flags is undefined, let F be pattern.[[OriginalFlags]].
        if (flags.is_undefined())
            flags_value = PrimitiveString::create(vm, regexp_pattern.flags());
        // c. Else, let F be flags.
        else
            flags_value = flags;
    }
    // 5. Else if patternIsRegExp is true, then
    else if (pattern_is_regexp) {
        // a. Let P be ? Get(pattern, "source").
        pattern_value = TRY(pattern.as_object().get(vm.names.source));

        // b. If flags is undefined, then
        if (flags.is_undefined()) {
            // i. Let F be ? Get(pattern, "flags").
            flags_value = TRY(pattern.as_object().get(vm.names.flags));
        }
        // c. Else, let F be flags.
        else {
            flags_value = flags;
        }
    }
    // 6. Else,
    else {
        // a. Let P be pattern.
        pattern_value = pattern;

        // b. Let F be flags.
        flags_value = flags;
    }

    // 7. Let O be ? RegExpAlloc(newTarget).
    auto regexp_object = TRY(regexp_alloc(vm, new_target));

    // 8. Return ? RegExpInitialize(O, P, F).
    return TRY(regexp_object->regexp_initialize(vm, pattern_value, flags_value));
}

// 22.2.5.2 get RegExp [ @@species ], https://tc39.es/ecma262/#sec-get-regexp-@@species
JS_DEFINE_NATIVE_FUNCTION(RegExpConstructor::symbol_species_getter)
{
    // 1. Return the this value.
    return vm.this_value();
}

// get RegExp.input, https://github.com/tc39/proposal-regexp-legacy-features#get-regexpinput
JS_DEFINE_NATIVE_FUNCTION(RegExpConstructor::input_getter)
{
    auto regexp_constructor = vm.current_realm()->intrinsics().regexp_constructor();

    // 1. Return ? GetLegacyRegExpStaticProperty(%RegExp%, this value, [[RegExpInput]]).
    auto property_getter = &RegExpLegacyStaticProperties::input;
    return TRY(get_legacy_regexp_static_property(vm, regexp_constructor, vm.this_value(), property_getter));
}

// get RegExp.$_, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp_
JS_DEFINE_NATIVE_FUNCTION(RegExpConstructor::input_alias_getter)
{
    // Keep the same implementation with `get RegExp.input`
    return input_getter(vm);
}

// set RegExp.input, https://github.com/tc39/proposal-regexp-legacy-features#set-regexpinput--val
JS_DEFINE_NATIVE_FUNCTION(RegExpConstructor::input_setter)
{
    auto regexp_constructor = vm.current_realm()->intrinsics().regexp_constructor();

    // 1. Perform ? SetLegacyRegExpStaticProperty(%RegExp%, this value, [[RegExpInput]], val).
    auto property_setter = &RegExpLegacyStaticProperties::set_input;
    TRY(set_legacy_regexp_static_property(vm, regexp_constructor, vm.this_value(), property_setter, vm.argument(0)));
    return js_undefined();
}

// set RegExp.$_, https://github.com/tc39/proposal-regexp-legacy-features#set-regexp_---val
JS_DEFINE_NATIVE_FUNCTION(RegExpConstructor::input_alias_setter)
{
    // Keep the same implementation with `set RegExp.input`
    return input_setter(vm);
}

// get RegExp.lastMatch, https://github.com/tc39/proposal-regexp-legacy-features#get-regexplastmatch
JS_DEFINE_NATIVE_FUNCTION(RegExpConstructor::last_match_getter)
{
    auto regexp_constructor = vm.current_realm()->intrinsics().regexp_constructor();

    // 1. Return ? GetLegacyRegExpStaticProperty(%RegExp%, this value, [[RegExpLastMatch]]).
    auto property_getter = &RegExpLegacyStaticProperties::last_match;
    return TRY(get_legacy_regexp_static_property(vm, regexp_constructor, vm.this_value(), property_getter));
}

// get RegExp.$&, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp
JS_DEFINE_NATIVE_FUNCTION(RegExpConstructor::last_match_alias_getter)
{
    // Keep the same implementation with `get RegExp.lastMatch`
    return last_match_getter(vm);
}

// get RegExp.lastParen, https://github.com/tc39/proposal-regexp-legacy-features#get-regexplastparen
JS_DEFINE_NATIVE_FUNCTION(RegExpConstructor::last_paren_getter)
{
    auto regexp_constructor = vm.current_realm()->intrinsics().regexp_constructor();

    // 1. Return ? GetLegacyRegExpStaticProperty(%RegExp%, this value, [[RegExpLastParen]]).
    auto property_getter = &RegExpLegacyStaticProperties::last_paren;
    return TRY(get_legacy_regexp_static_property(vm, regexp_constructor, vm.this_value(), property_getter));
}

// get RegExp.$+, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp-1
JS_DEFINE_NATIVE_FUNCTION(RegExpConstructor::last_paren_alias_getter)
{
    // Keep the same implementation with `get RegExp.lastParen`
    return last_paren_getter(vm);
}

// get RegExp.leftContext, https://github.com/tc39/proposal-regexp-legacy-features#get-regexpleftcontext
JS_DEFINE_NATIVE_FUNCTION(RegExpConstructor::left_context_getter)
{
    auto regexp_constructor = vm.current_realm()->intrinsics().regexp_constructor();

    // 1. Return ? GetLegacyRegExpStaticProperty(%RegExp%, this value, [[RegExpLeftContext]]).
    auto property_getter = &RegExpLegacyStaticProperties::left_context;
    return TRY(get_legacy_regexp_static_property(vm, regexp_constructor, vm.this_value(), property_getter));
}

// get RegExp.$`, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp-2
JS_DEFINE_NATIVE_FUNCTION(RegExpConstructor::left_context_alias_getter)
{
    // Keep the same implementation with `get RegExp.leftContext`
    return left_context_getter(vm);
}

// get RegExp.rightContext, https://github.com/tc39/proposal-regexp-legacy-features#get-regexprightcontext
JS_DEFINE_NATIVE_FUNCTION(RegExpConstructor::right_context_getter)
{
    auto regexp_constructor = vm.current_realm()->intrinsics().regexp_constructor();

    // 1. Return ? GetLegacyRegExpStaticProperty(%RegExp%, this value, [[RegExpRightContext]]).
    auto property_getter = &RegExpLegacyStaticProperties::right_context;
    return TRY(get_legacy_regexp_static_property(vm, regexp_constructor, vm.this_value(), property_getter));
}

// get RegExp.$', https://github.com/tc39/proposal-regexp-legacy-features#get-regexp-3
JS_DEFINE_NATIVE_FUNCTION(RegExpConstructor::right_context_alias_getter)
{
    // Keep the same implementation with `get RegExp.rightContext`
    return right_context_getter(vm);
}

#define DEFINE_REGEXP_GROUP_GETTER(n)                                                                            \
    JS_DEFINE_NATIVE_FUNCTION(RegExpConstructor::group_##n##_getter)                                             \
    {                                                                                                            \
        auto regexp_constructor = vm.current_realm()->intrinsics().regexp_constructor();                         \
                                                                                                                 \
        /* 1. Return ? GetLegacyRegExpStaticProperty(%RegExp%, this value, [[RegExpParen##n##]]).*/              \
        auto property_getter = &RegExpLegacyStaticProperties::$##n;                                              \
        return TRY(get_legacy_regexp_static_property(vm, regexp_constructor, vm.this_value(), property_getter)); \
    }

// get RegExp.$1, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp1
DEFINE_REGEXP_GROUP_GETTER(1);
// get RegExp.$2, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp2
DEFINE_REGEXP_GROUP_GETTER(2);
// get RegExp.$3, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp3
DEFINE_REGEXP_GROUP_GETTER(3);
// get RegExp.$4, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp4
DEFINE_REGEXP_GROUP_GETTER(4);
// get RegExp.$5, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp5
DEFINE_REGEXP_GROUP_GETTER(5);
// get RegExp.$6, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp6
DEFINE_REGEXP_GROUP_GETTER(6);
// get RegExp.$7, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp7
DEFINE_REGEXP_GROUP_GETTER(7);
// get RegExp.$8, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp8
DEFINE_REGEXP_GROUP_GETTER(8);
// get RegExp.$9, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp9
DEFINE_REGEXP_GROUP_GETTER(9);

#undef DEFINE_REGEXP_GROUP_GETTER

}
