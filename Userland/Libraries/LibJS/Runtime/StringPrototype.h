/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf8View.h>
#include <LibJS/Runtime/StringObject.h>

namespace JS {

struct CodePoint {
    bool is_unpaired_surrogate { false };
    u32 code_point { 0 };
    size_t code_unit_count { 0 };
};

Optional<size_t> string_index_of(Utf16View const& string, Utf16View const& search_value, size_t from_index);
CodePoint code_point_at(Utf16View const& string, size_t position);
String to_well_formed_string(Utf16String const&);
static constexpr Utf8View whitespace_characters = Utf8View("\x09\x0A\x0B\x0C\x0D\x20\xC2\xA0\xE1\x9A\x80\xE2\x80\x80\xE2\x80\x81\xE2\x80\x82\xE2\x80\x83\xE2\x80\x84\xE2\x80\x85\xE2\x80\x86\xE2\x80\x87\xE2\x80\x88\xE2\x80\x89\xE2\x80\x8A\xE2\x80\xAF\xE2\x81\x9F\xE3\x80\x80\xE2\x80\xA8\xE2\x80\xA9\xEF\xBB\xBF"sv);
ThrowCompletionOr<String> trim_string(VM&, Value string, TrimMode where);

class StringPrototype final : public StringObject {
    JS_OBJECT(StringPrototype, StringObject);
    JS_DECLARE_ALLOCATOR(StringPrototype);

public:
    explicit StringPrototype(Realm&);
    virtual void initialize(Realm&) override;
    virtual ~StringPrototype() override = default;

private:
    JS_DECLARE_NATIVE_FUNCTION(at);
    JS_DECLARE_NATIVE_FUNCTION(char_at);
    JS_DECLARE_NATIVE_FUNCTION(char_code_at);
    JS_DECLARE_NATIVE_FUNCTION(code_point_at);
    JS_DECLARE_NATIVE_FUNCTION(concat);
    JS_DECLARE_NATIVE_FUNCTION(ends_with);
    JS_DECLARE_NATIVE_FUNCTION(includes);
    JS_DECLARE_NATIVE_FUNCTION(index_of);
    JS_DECLARE_NATIVE_FUNCTION(is_well_formed);
    JS_DECLARE_NATIVE_FUNCTION(last_index_of);
    JS_DECLARE_NATIVE_FUNCTION(locale_compare);
    JS_DECLARE_NATIVE_FUNCTION(match);
    JS_DECLARE_NATIVE_FUNCTION(match_all);
    JS_DECLARE_NATIVE_FUNCTION(normalize);
    JS_DECLARE_NATIVE_FUNCTION(pad_end);
    JS_DECLARE_NATIVE_FUNCTION(pad_start);
    JS_DECLARE_NATIVE_FUNCTION(repeat);
    JS_DECLARE_NATIVE_FUNCTION(replace);
    JS_DECLARE_NATIVE_FUNCTION(replace_all);
    JS_DECLARE_NATIVE_FUNCTION(search);
    JS_DECLARE_NATIVE_FUNCTION(slice);
    JS_DECLARE_NATIVE_FUNCTION(split);
    JS_DECLARE_NATIVE_FUNCTION(starts_with);
    JS_DECLARE_NATIVE_FUNCTION(substring);
    JS_DECLARE_NATIVE_FUNCTION(to_locale_lowercase);
    JS_DECLARE_NATIVE_FUNCTION(to_locale_uppercase);
    JS_DECLARE_NATIVE_FUNCTION(to_lowercase);
    JS_DECLARE_NATIVE_FUNCTION(to_string);
    JS_DECLARE_NATIVE_FUNCTION(to_uppercase);
    JS_DECLARE_NATIVE_FUNCTION(to_well_formed);
    JS_DECLARE_NATIVE_FUNCTION(trim);
    JS_DECLARE_NATIVE_FUNCTION(trim_end);
    JS_DECLARE_NATIVE_FUNCTION(trim_start);
    JS_DECLARE_NATIVE_FUNCTION(value_of);
    JS_DECLARE_NATIVE_FUNCTION(symbol_iterator);

    JS_DECLARE_NATIVE_FUNCTION(substr);
    JS_DECLARE_NATIVE_FUNCTION(anchor);
    JS_DECLARE_NATIVE_FUNCTION(big);
    JS_DECLARE_NATIVE_FUNCTION(blink);
    JS_DECLARE_NATIVE_FUNCTION(bold);
    JS_DECLARE_NATIVE_FUNCTION(fixed);
    JS_DECLARE_NATIVE_FUNCTION(fontcolor);
    JS_DECLARE_NATIVE_FUNCTION(fontsize);
    JS_DECLARE_NATIVE_FUNCTION(italics);
    JS_DECLARE_NATIVE_FUNCTION(link);
    JS_DECLARE_NATIVE_FUNCTION(small);
    JS_DECLARE_NATIVE_FUNCTION(strike);
    JS_DECLARE_NATIVE_FUNCTION(sub);
    JS_DECLARE_NATIVE_FUNCTION(sup);
};

}
