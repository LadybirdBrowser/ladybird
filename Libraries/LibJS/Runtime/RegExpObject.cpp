/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Function.h>
#include <AK/UnicodeUtils.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/RegExpConstructor.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/StringPrototype.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Token.h>

namespace JS {

GC_DEFINE_ALLOCATOR(RegExpObject);

namespace {

enum class RegExpNameElementKind {
    CodePoint,
    HighSurrogate,
    LowSurrogate,
};

enum class RegExpNameElementOrigin {
    Literal,
    FixedEscape,
    BracedEscape,
};

struct RegExpNameElement {
    RegExpNameElementKind kind;
    RegExpNameElementOrigin origin;
    size_t next_index { 0 };
};

static ParseRegexPatternError invalid_group_name_error()
{
    return ParseRegexPatternError { "invalid group name"_string };
}

static ErrorOr<RegExpNameElement, ParseRegexPatternError> parse_regexp_name_element(Utf16View const& pattern, size_t index)
{
    auto const length = pattern.length_in_code_units();
    if (index >= length)
        return invalid_group_name_error();

    auto code_unit = pattern.code_unit_at(index);
    if (code_unit != '\\') {
        if (AK::UnicodeUtils::is_utf16_high_surrogate(code_unit)) {
            if (index + 1 < length) {
                auto next_code_unit = pattern.code_unit_at(index + 1);
                if (AK::UnicodeUtils::is_utf16_low_surrogate(next_code_unit))
                    return RegExpNameElement { RegExpNameElementKind::CodePoint, RegExpNameElementOrigin::Literal, index + 2 };
            }
            return RegExpNameElement { RegExpNameElementKind::HighSurrogate, RegExpNameElementOrigin::Literal, index + 1 };
        }
        if (AK::UnicodeUtils::is_utf16_low_surrogate(code_unit))
            return RegExpNameElement { RegExpNameElementKind::LowSurrogate, RegExpNameElementOrigin::Literal, index + 1 };
        return RegExpNameElement { RegExpNameElementKind::CodePoint, RegExpNameElementOrigin::Literal, index + 1 };
    }

    if (index + 1 >= length || pattern.code_unit_at(index + 1) != 'u')
        return invalid_group_name_error();

    auto escape_index = index + 2;
    if (escape_index < length && pattern.code_unit_at(escape_index) == '{') {
        ++escape_index;

        u32 value = 0;
        size_t digits = 0;
        while (escape_index < length && pattern.code_unit_at(escape_index) != '}') {
            auto digit = pattern.code_unit_at(escape_index);
            if (!is_ascii_hex_digit(digit))
                return invalid_group_name_error();
            value = value * 16 + parse_ascii_hex_digit(digit);
            if (value > 0x10FFFF)
                return invalid_group_name_error();
            ++digits;
            ++escape_index;
        }

        if (digits == 0 || escape_index >= length || pattern.code_unit_at(escape_index) != '}')
            return invalid_group_name_error();

        ++escape_index;
        if (AK::UnicodeUtils::is_utf16_high_surrogate(value))
            return RegExpNameElement { RegExpNameElementKind::HighSurrogate, RegExpNameElementOrigin::BracedEscape, escape_index };
        if (AK::UnicodeUtils::is_utf16_low_surrogate(value))
            return RegExpNameElement { RegExpNameElementKind::LowSurrogate, RegExpNameElementOrigin::BracedEscape, escape_index };
        return RegExpNameElement { RegExpNameElementKind::CodePoint, RegExpNameElementOrigin::BracedEscape, escape_index };
    }

    if (escape_index + 4 > length)
        return invalid_group_name_error();

    u32 value = 0;
    for (size_t offset = 0; offset < 4; ++offset) {
        auto digit = pattern.code_unit_at(escape_index + offset);
        if (!is_ascii_hex_digit(digit))
            return invalid_group_name_error();
        value = value * 16 + parse_ascii_hex_digit(digit);
    }

    auto next_index = escape_index + 4;
    if (AK::UnicodeUtils::is_utf16_high_surrogate(value))
        return RegExpNameElement { RegExpNameElementKind::HighSurrogate, RegExpNameElementOrigin::FixedEscape, next_index };
    if (AK::UnicodeUtils::is_utf16_low_surrogate(value))
        return RegExpNameElement { RegExpNameElementKind::LowSurrogate, RegExpNameElementOrigin::FixedEscape, next_index };
    return RegExpNameElement { RegExpNameElementKind::CodePoint, RegExpNameElementOrigin::FixedEscape, next_index };
}

static ErrorOr<size_t, ParseRegexPatternError> validate_regexp_name_surrogates(Utf16View const& pattern, size_t name_start)
{
    auto const length = pattern.length_in_code_units();
    auto index = name_start;

    while (index < length) {
        if (pattern.code_unit_at(index) == '>')
            return index + 1;

        auto element = TRY(parse_regexp_name_element(pattern, index));
        if (element.kind == RegExpNameElementKind::CodePoint) {
            index = element.next_index;
            continue;
        }

        if (element.kind == RegExpNameElementKind::LowSurrogate)
            return invalid_group_name_error();

        auto next_element = TRY(parse_regexp_name_element(pattern, element.next_index));
        if (next_element.kind != RegExpNameElementKind::LowSurrogate)
            return invalid_group_name_error();
        if (element.origin != next_element.origin)
            return invalid_group_name_error();
        if (element.origin == RegExpNameElementOrigin::BracedEscape)
            return invalid_group_name_error();

        index = next_element.next_index;
    }

    return invalid_group_name_error();
}

static bool pattern_has_named_capture_groups(Utf16View const& pattern)
{
    auto const length = pattern.length_in_code_units();
    bool in_character_class = false;

    for (size_t index = 0; index < length; ++index) {
        auto code_unit = pattern.code_unit_at(index);

        if (code_unit == '\\') {
            if (index + 1 < length)
                ++index;
            continue;
        }

        if (code_unit == '[' && !in_character_class) {
            in_character_class = true;
            continue;
        }

        if (code_unit == ']' && in_character_class) {
            in_character_class = false;
            continue;
        }

        if (in_character_class)
            continue;

        if (code_unit == '(' && index + 2 < length && pattern.code_unit_at(index + 1) == '?' && pattern.code_unit_at(index + 2) == '<') {
            if (index + 3 >= length || (pattern.code_unit_at(index + 3) != '=' && pattern.code_unit_at(index + 3) != '!'))
                return true;
        }
    }

    return false;
}

static ErrorOr<void, ParseRegexPatternError> validate_named_group_name_surrogates(Utf16View const& pattern, bool unicode_aware)
{
    auto const length = pattern.length_in_code_units();
    bool in_character_class = false;
    bool has_named_groups_or_unicode = unicode_aware || pattern_has_named_capture_groups(pattern);

    for (size_t index = 0; index < length; ++index) {
        auto code_unit = pattern.code_unit_at(index);

        if (code_unit == '\\') {
            if (has_named_groups_or_unicode && !in_character_class && index + 2 < length && pattern.code_unit_at(index + 1) == 'k' && pattern.code_unit_at(index + 2) == '<') {
                index = TRY(validate_regexp_name_surrogates(pattern, index + 3)) - 1;
                continue;
            }

            if (index + 1 < length)
                ++index;
            continue;
        }

        if (code_unit == '[' && !in_character_class) {
            in_character_class = true;
            continue;
        }

        if (code_unit == ']' && in_character_class) {
            in_character_class = false;
            continue;
        }

        if (in_character_class)
            continue;

        if (code_unit == '(' && index + 2 < length && pattern.code_unit_at(index + 1) == '?' && pattern.code_unit_at(index + 2) == '<') {
            if (index + 3 < length && pattern.code_unit_at(index + 3) != '=' && pattern.code_unit_at(index + 3) != '!') {
                index = TRY(validate_regexp_name_surrogates(pattern, index + 3)) - 1;
            }
        }
    }

    return {};
}

}

static Result<RegExpObject::Flags, String> validate_flags(Utf16View const& flags)
{
    bool seen[128] {};
    RegExpObject::Flags flag_bits = static_cast<RegExpObject::Flags>(0);

    for (size_t index = 0; index < flags.length_in_code_units(); ++index) {
        auto ch = flags.code_unit_at(index);

        switch (ch) {
#define __JS_ENUMERATE(FlagName, flagName, flag_name, flag_char)                              \
    case #flag_char[0]:                                                                       \
        if (seen[ch])                                                                         \
            return MUST(String::formatted(ErrorType::RegExpObjectRepeatedFlag.format(), ch)); \
        seen[ch] = true;                                                                      \
        flag_bits |= RegExpObject::Flags::FlagName;                                           \
        break;
            JS_ENUMERATE_REGEXP_FLAGS
#undef __JS_ENUMERATE
        default:
            return MUST(String::formatted(ErrorType::RegExpObjectBadFlag.format(), ch));
        }
    }

    if (has_flag(flag_bits, RegExpObject::Flags::Unicode) && has_flag(flag_bits, RegExpObject::Flags::UnicodeSets))
        return MUST(String::formatted(ErrorType::RegExpObjectIncompatibleFlags.format(), 'u', 'v'));

    return flag_bits;
}

// 22.2.3.4 Static Semantics: ParsePattern ( patternText, u, v ), https://tc39.es/ecma262/#sec-parsepattern
ErrorOr<String, ParseRegexPatternError> parse_regex_pattern(Utf16View const& pattern, bool unicode, bool unicode_sets)
{
    if (unicode && unicode_sets)
        return ParseRegexPatternError { MUST(String::formatted(ErrorType::RegExpObjectIncompatibleFlags.format(), 'u', 'v')) };

    TRY(validate_named_group_name_surrogates(pattern, unicode || unicode_sets));

    StringBuilder builder;

    auto previous_code_unit_was_backslash = false;
    for (size_t i = 0; i < pattern.length_in_code_units(); ++i) {
        u16 code_unit = pattern.code_unit_at(i);

        if (code_unit > 0x7f) {
            // Incorrectly escaping this code unit will result in a wildly different regex than intended
            // as we're converting <c> to <\uhhhh>, which would turn into <\\uhhhh> if (incorrectly) escaped again,
            // leading to a matcher for the literal string "\uhhhh" instead of the intended code unit <c>.
            // As such, we're going to remove the (invalid) backslash and pretend it never existed.
            if (!previous_code_unit_was_backslash)
                builder.append('\\');

            if ((unicode || unicode_sets) && AK::UnicodeUtils::is_utf16_high_surrogate(code_unit) && i + 1 < pattern.length_in_code_units()) {
                u16 next_code_unit = pattern.code_unit_at(i + 1);
                if (AK::UnicodeUtils::is_utf16_low_surrogate(next_code_unit)) {
                    u32 combined = AK::UnicodeUtils::decode_utf16_surrogate_pair(code_unit, next_code_unit);
                    builder.appendff("u{{{:x}}}", combined);
                    ++i;
                    previous_code_unit_was_backslash = false;
                    continue;
                }
            }

            if (unicode || unicode_sets)
                builder.appendff("u{{{:04x}}}", code_unit);
            else
                builder.appendff("u{:04x}", code_unit);
        } else {
            builder.append_code_point(code_unit);
        }

        if (code_unit == '\\')
            previous_code_unit_was_backslash = !previous_code_unit_was_backslash;
        else
            previous_code_unit_was_backslash = false;
    }

    return builder.to_string_without_validation();
}

// 22.2.3.4 Static Semantics: ParsePattern ( patternText, u, v ), https://tc39.es/ecma262/#sec-parsepattern
ThrowCompletionOr<String> parse_regex_pattern(VM& vm, Utf16View const& pattern, bool unicode, bool unicode_sets)
{
    auto result = parse_regex_pattern(pattern, unicode, unicode_sets);
    if (result.is_error())
        return vm.throw_completion<JS::SyntaxError>(result.release_error().error);

    return result.release_value();
}

GC::Ref<RegExpObject> RegExpObject::create(Realm& realm)
{
    return realm.create<RegExpObject>(realm.intrinsics().regexp_prototype());
}

GC::Ref<RegExpObject> RegExpObject::create(Realm& realm, Utf16String pattern, Utf16String flags)
{
    return realm.create<RegExpObject>(move(pattern), move(flags), realm.intrinsics().regexp_prototype());
}

RegExpObject::RegExpObject(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
}

static RegExpObject::Flags to_flag_bits(Utf16View const& flags)
{
    RegExpObject::Flags flag_bits = static_cast<RegExpObject::Flags>(0);

    for (size_t i = 0; i < flags.length_in_code_units(); ++i) {
        auto ch = flags.code_unit_at(i);
        switch (ch) {
#define __JS_ENUMERATE(FlagName, flagName, flag_name, flag_char) \
    case #flag_char[0]:                                          \
        flag_bits |= RegExpObject::Flags::FlagName;              \
        break;
            JS_ENUMERATE_REGEXP_FLAGS
#undef __JS_ENUMERATE
        default:
            break;
        }
    }
    return flag_bits;
}

RegExpObject::RegExpObject(Utf16String pattern, Utf16String flags, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_pattern(move(pattern))
    , m_flags(move(flags))
    , m_flag_bits(to_flag_bits(m_flags))
{
}

void RegExpObject::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    define_direct_property(vm.names.lastIndex, Value(0), Attribute::Writable);
}

// 22.2.3.3 RegExpInitialize ( obj, pattern, flags ), https://tc39.es/ecma262/#sec-regexpinitialize
ThrowCompletionOr<GC::Ref<RegExpObject>> RegExpObject::regexp_initialize(VM& vm, Value pattern_value, Value flags_value)
{
    // Invalidate the cached compiled regex since the pattern/flags may change.
    m_cached_regex = nullptr;

    // 1. If pattern is undefined, let P be the empty String.
    // 2. Else, let P be ? ToString(pattern).
    auto pattern = pattern_value.is_undefined()
        ? Utf16String {}
        : TRY(pattern_value.to_utf16_string(vm));

    // 3. If flags is undefined, let F be the empty String.
    // 4. Else, let F be ? ToString(flags).
    auto flags = flags_value.is_undefined()
        ? Utf16String {}
        : TRY(flags_value.to_utf16_string(vm));

    // 5. If F contains any code unit other than "d", "g", "i", "m", "s", "u", "v", or "y", or if F contains any code unit more than once, throw a SyntaxError exception.
    // 6. If F contains "i", let i be true; else let i be false.
    // 7. If F contains "m", let m be true; else let m be false.
    // 8. If F contains "s", let s be true; else let s be false.
    // 9. If F contains "u", let u be true; else let u be false.
    // 10. If F contains "v", let v be true; else let v be false.
    auto validated_flags_or_error = validate_flags(flags);
    if (validated_flags_or_error.is_error())
        return vm.throw_completion<SyntaxError>(validated_flags_or_error.release_error());
    auto flag_bits = validated_flags_or_error.release_value();
    bool unicode = has_flag(flag_bits, Flags::Unicode);
    bool unicode_sets = has_flag(flag_bits, Flags::UnicodeSets);

    auto parsed_pattern = String {};

    // Convert UTF-16 pattern to UTF-8 (with escape normalization for non-ASCII).
    if (!pattern.is_empty()) {
        auto result = parse_regex_pattern(pattern, unicode, unicode_sets);
        if (result.is_error())
            return vm.throw_completion<SyntaxError>(ErrorType::RegExpCompileError, result.release_error().error);
        parsed_pattern = result.release_value();
    }

    // 11. If u is true and v is true, throw a SyntaxError exception.
    // NB: Already handled by validate_flags above.

    // Validate by trial-compiling the pattern.
    regex::ECMAScriptCompileFlags compile_flags {};
    compile_flags.global = has_flag(flag_bits, Flags::Global);
    compile_flags.ignore_case = has_flag(flag_bits, Flags::IgnoreCase);
    compile_flags.multiline = has_flag(flag_bits, Flags::Multiline);
    compile_flags.dot_all = has_flag(flag_bits, Flags::DotAll);
    compile_flags.unicode = unicode;
    compile_flags.unicode_sets = unicode_sets;
    compile_flags.sticky = has_flag(flag_bits, Flags::Sticky);

    auto compiled = regex::ECMAScriptRegex::compile(parsed_pattern.bytes_as_string_view(), compile_flags);
    if (compiled.is_error())
        return vm.throw_completion<SyntaxError>(ErrorType::RegExpCompileError, compiled.release_error());

    // 16. Set obj.[[OriginalSource]] to P.
    m_pattern = move(pattern);

    // 17. Set obj.[[OriginalFlags]] to F.
    m_flag_bits = to_flag_bits(flags);
    m_flags = move(flags);

    // 18. Let capturingGroupsCount be CountLeftCapturingParensWithin(parseResult).
    // 19. Let rer be the RegExp Record { [[IgnoreCase]]: i, [[Multiline]]: m, [[DotAll]]: s, [[Unicode]]: u, [[CapturingGroupsCount]]: capturingGroupsCount }.
    // 20. Set obj.[[RegExpRecord]] to rer.
    // 21. Set obj.[[RegExpMatcher]] to CompilePattern of parseResult with argument rer.

    // 22. Perform ? Set(obj, "lastIndex", +0𝔽, true).
    TRY(set(vm.names.lastIndex, Value(0), Object::ShouldThrowExceptions::Yes));

    // 23. Return obj.
    return GC::Ref { *this };
}

// 22.2.6.13.1 EscapeRegExpPattern ( P, F ), https://tc39.es/ecma262/#sec-escaperegexppattern
String RegExpObject::escape_regexp_pattern() const
{
    // 1. Let S be a String in the form of a Pattern[~UnicodeMode] (Pattern[+UnicodeMode] if F contains "u") equivalent
    //    to P interpreted as UTF-16 encoded Unicode code points (6.1.4), in which certain code points are escaped as
    //    described below. S may or may not be identical to P; however, the Abstract Closure that would result from
    //    evaluating S as a Pattern[~UnicodeMode] (Pattern[+UnicodeMode] if F contains "u") must behave identically to
    //    the Abstract Closure given by the constructed object's [[RegExpMatcher]] internal slot. Multiple calls to
    //    this abstract operation using the same values for P and F must produce identical results.
    // 2. The code points / or any LineTerminator occurring in the pattern shall be escaped in S as necessary to ensure
    //    that the string-concatenation of "/", S, "/", and F can be parsed (in an appropriate lexical context) as a
    //    RegularExpressionLiteral that behaves identically to the constructed regular expression. For example, if P is
    //    "/", then S could be "\/" or "\u002F", among other possibilities, but not "/", because /// followed by F
    //    would be parsed as a SingleLineComment rather than a RegularExpressionLiteral. If P is the empty String, this
    //    specification can be met by letting S be "(?:)".
    // 3. Return S.
    if (m_pattern.is_empty())
        return "(?:)"_string;

    // FIXME: Check the 'u' and 'v' flags and escape accordingly
    StringBuilder builder;
    auto escaped = false;
    auto in_character_class = false;

    for (auto code_point : m_pattern) {
        if (escaped) {
            escaped = false;
            builder.append_code_point('\\');

            switch (code_point) {
            case '\n':
                builder.append_code_point('n');
                break;
            case '\r':
                builder.append_code_point('r');
                break;
            case LINE_SEPARATOR:
                builder.append("u2028"sv);
                break;
            case PARAGRAPH_SEPARATOR:
                builder.append("u2029"sv);
                break;
            default:
                builder.append_code_point(code_point);
                break;
            }
            continue;
        }

        if (code_point == '\\') {
            escaped = true;
            continue;
        }

        if (code_point == '[') {
            in_character_class = true;
        } else if (code_point == ']') {
            in_character_class = false;
        }

        switch (code_point) {
        case '/':
            if (in_character_class)
                builder.append_code_point('/');
            else
                builder.append("\\/"sv);
            break;
        case '\n':
            builder.append("\\n"sv);
            break;
        case '\r':
            builder.append("\\r"sv);
            break;
        case LINE_SEPARATOR:
            builder.append("\\u2028"sv);
            break;
        case PARAGRAPH_SEPARATOR:
            builder.append("\\u2029"sv);
            break;
        default:
            builder.append_code_point(code_point);
            break;
        }
    }

    return builder.to_string_without_validation();
}

void RegExpObject::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
}

// 22.2.3.1 RegExpCreate ( P, F ), https://tc39.es/ecma262/#sec-regexpcreate
ThrowCompletionOr<GC::Ref<RegExpObject>> regexp_create(VM& vm, Value pattern, Value flags)
{
    auto& realm = *vm.current_realm();

    // 1. Let obj be ! RegExpAlloc(%RegExp%).
    auto regexp_object = MUST(regexp_alloc(vm, realm.intrinsics().regexp_constructor()));

    // 2. Return ? RegExpInitialize(obj, P, F).
    return TRY(regexp_object->regexp_initialize(vm, pattern, flags));
}

// 22.2.3.2 RegExpAlloc ( newTarget ), https://tc39.es/ecma262/#sec-regexpalloc
// 22.2.3.2 RegExpAlloc ( newTarget ), https://github.com/tc39/proposal-regexp-legacy-features#regexpalloc--newtarget-
ThrowCompletionOr<GC::Ref<RegExpObject>> regexp_alloc(VM& vm, FunctionObject& new_target)
{
    // 1. Let obj be ? OrdinaryCreateFromConstructor(newTarget, "%RegExp.prototype%", « [[OriginalSource]], [[OriginalFlags]], [[RegExpRecord]], [[RegExpMatcher]] »).
    auto regexp_object = TRY(ordinary_create_from_constructor<RegExpObject>(vm, new_target, &Intrinsics::regexp_prototype));

    // 2. Let thisRealm be the current Realm Record.
    auto& this_realm = *vm.current_realm();

    // 3. Set the value of obj’s [[Realm]] internal slot to thisRealm.
    regexp_object->set_realm(this_realm);

    // 4. If SameValue(newTarget, thisRealm.[[Intrinsics]].[[%RegExp%]]) is true, then
    if (same_value(&new_target, this_realm.intrinsics().regexp_constructor())) {
        // i. Set the value of obj’s [[LegacyFeaturesEnabled]] internal slot to true.
        regexp_object->set_legacy_features_enabled(true);
    }
    // 5. Else,
    else {
        // i. Set the value of obj’s [[LegacyFeaturesEnabled]] internal slot to false.
        regexp_object->set_legacy_features_enabled(false);
    }

    // 6. Perform ! DefinePropertyOrThrow(obj, "lastIndex", PropertyDescriptor { [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: false }).
    PropertyDescriptor descriptor { .writable = true, .enumerable = false, .configurable = false };
    MUST(regexp_object->define_property_or_throw(vm.names.lastIndex, descriptor));

    // 7. Return obj.
    return regexp_object;
}

}
