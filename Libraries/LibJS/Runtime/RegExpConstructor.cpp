/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Find.h>
#include <LibJS/Lexer.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/RegExpConstructor.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

GC_DEFINE_ALLOCATOR(RegExpConstructor);

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

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.escape, escape, 1, attr);
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
ThrowCompletionOr<GC::Ref<Object>> RegExpConstructor::construct(FunctionObject& new_target)
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

// 22.2.5.1.1 EncodeForRegExpEscape ( c ), https://tc39.es/proposal-regex-escaping/#sec-encodeforregexpescape
static String encode_for_regexp_escape(u32 code_point)
{
    // https://tc39.es/ecma262/#table-controlescape-code-point-values
    // Table 63: ControlEscape Code Point Values
    struct ControlEscape {
        u32 code_point { 0 };
        char control_escape { 0 };
    };
    static constexpr auto control_escapes = to_array<ControlEscape>({
        { 0x09, 't' },
        { 0x0A, 'n' },
        { 0x0B, 'v' },
        { 0x0C, 'f' },
        { 0x0D, 'r' },
    });

    // 1. If c is matched by SyntaxCharacter or c is U+002F (SOLIDUS), then
    if (JS::is_syntax_character(code_point) || code_point == '/') {
        // a. Return the string-concatenation of 0x005C (REVERSE SOLIDUS) and UTF16EncodeCodePoint(c).
        return MUST(String::formatted("\\{}", String::from_code_point(code_point)));
    }

    // 2. Else if c is the code point listed in some cell of the “Code Point” column of Table 63, then
    auto it = find_if(control_escapes.begin(), control_escapes.end(), [&](auto const& escape) {
        return escape.code_point == code_point;
    });

    if (it != control_escapes.end()) {
        // a. Return the string-concatenation of 0x005C (REVERSE SOLIDUS) and the string in the “ControlEscape” column
        //    of the row whose “Code Point” column contains c.
        return MUST(String::formatted("\\{}", it->control_escape));
    }

    // 3. Let otherPunctuators be the string-concatenation of ",-=<>#&!%:;@~'`" and the code unit 0x0022 (QUOTATION MARK).
    // 4. Let toEscape be StringToCodePoints(otherPunctuators).
    static constexpr Utf8View to_escape { ",-=<>#&!%:;@~'`\""sv };

    // 5. If toEscape contains c, c is matched by either WhiteSpace or LineTerminator, or c has the same numeric value
    //    as a leading surrogate or trailing surrogate, then
    if (to_escape.contains(code_point) || JS::is_whitespace(code_point) || JS::is_line_terminator(code_point) || is_unicode_surrogate(code_point)) {
        // a. Let cNum be the numeric value of c.
        // b. If cNum ≤ 0xFF, then
        if (code_point <= 0xFF) {
            // i. Let hex be Number::toString(𝔽(cNum), 16).
            // ii. Return the string-concatenation of the code unit 0x005C (REVERSE SOLIDUS), "x", and
            //     StringPad(hex, 2, "0", START).
            return MUST(String::formatted("\\x{:02x}", code_point));
        }

        // c. Let escaped be the empty String.
        // d. Let codeUnits be UTF16EncodeCodePoint(c).
        // e. For each code unit cu of codeUnits, do
        //     i. Set escaped to the string-concatenation of escaped and UnicodeEscape(cu).
        // f. Return escaped.
        return MUST(String::formatted("\\u{:04x}", code_point));
    }

    // 6. Return UTF16EncodeCodePoint(c).
    return String::from_code_point(code_point);
}

// 22.2.5.1 RegExp.escape ( S ), https://tc39.es/proposal-regex-escaping/
JS_DEFINE_NATIVE_FUNCTION(RegExpConstructor::escape)
{
    auto string = vm.argument(0);

    // 1. If S is not a String, throw a TypeError exception.
    if (!string.is_string())
        return vm.throw_completion<TypeError>(ErrorType::NotAString, string);

    // 2. Let escaped be the empty String.
    StringBuilder escaped(string.as_string().utf8_string().byte_count());

    // 3. Let cpList be StringToCodePoints(S).
    auto code_point_list = string.as_string().utf8_string();

    // 4. For each code point c of cpList, do
    for (auto code_point : code_point_list.code_points()) {
        // a. If escaped is the empty String and c is matched by either DecimalDigit or AsciiLetter, then
        if (escaped.is_empty() && is_ascii_alphanumeric(code_point)) {
            // i. NOTE: Escaping a leading digit ensures that output corresponds with pattern text which may be used
            //    after a \0 character escape or a DecimalEscape such as \1 and still match S rather than be interpreted
            //    as an extension of the preceding escape sequence. Escaping a leading ASCII letter does the same for
            //    the context after \c.

            // ii. Let numericValue be the numeric value of c.
            // iii. Let hex be Number::toString(𝔽(numericValue), 16).
            // iv. Assert: The length of hex is 2.
            // v. Set escaped to the string-concatenation of the code unit 0x005C (REVERSE SOLIDUS), "x", and hex.
            escaped.appendff("\\x{:02x}", code_point);
        }
        // b. Else,
        else {
            // i. Set escaped to the string-concatenation of escaped and EncodeForRegExpEscape(c).
            escaped.append(encode_for_regexp_escape(code_point));
        }
    }

    // 5. Return escaped.
    return JS::PrimitiveString::create(vm, MUST(escaped.to_string()));
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
