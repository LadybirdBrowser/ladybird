/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Forward.h"
#include "RegexOptions.h"
#include <AK/Error.h>

#include <AK/ByteString.h>
#include <AK/COWVector.h>
#include <AK/FlyString.h>
#include <AK/MemMem.h>
#include <AK/RedBlackTree.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Utf16View.h>
#include <AK/Utf32View.h>
#include <AK/Utf8View.h>
#include <AK/Variant.h>
#include <AK/Vector.h>

namespace regex {

class RegexStringView {
public:
    RegexStringView() = default;

    RegexStringView(ByteString const& string)
        : m_view(string.view())
    {
    }

    RegexStringView(String const& string)
        : m_view(string.bytes_as_string_view())
    {
    }

    RegexStringView(StringView const view)
        : m_view(view)
    {
    }

    RegexStringView(Utf32View view)
        : m_view(view)
    {
    }

    RegexStringView(Utf16View view)
        : m_view(view)
    {
    }

    RegexStringView(Utf8View view)
        : m_view(view)
    {
    }

    RegexStringView(ByteString&&) = delete;
    RegexStringView(String&&) = delete;

    bool is_string_view() const
    {
        return m_view.has<StringView>();
    }

    StringView string_view() const
    {
        return m_view.get<StringView>();
    }

    Utf32View const& u32_view() const
    {
        return m_view.get<Utf32View>();
    }

    Utf16View const& u16_view() const
    {
        return m_view.get<Utf16View>();
    }

    Utf8View const& u8_view() const
    {
        return m_view.get<Utf8View>();
    }

    bool unicode() const { return m_unicode; }
    void set_unicode(bool unicode) { m_unicode = unicode; }

    bool is_empty() const
    {
        return m_view.visit([](auto& view) { return view.is_empty(); });
    }

    bool is_null() const
    {
        return m_view.visit([](auto& view) { return view.is_null(); });
    }

    size_t length() const
    {
        if (unicode()) {
            return m_view.visit(
                [](Utf16View const& view) { return view.length_in_code_points(); },
                [](auto const& view) { return view.length(); });
        }

        return length_in_code_units();
    }

    size_t length_in_code_units() const
    {
        return m_view.visit(
            [](Utf16View const& view) { return view.length_in_code_units(); },
            [](Utf8View const& view) { return view.byte_length(); },
            [](auto const& view) { return view.length(); });
    }

    size_t length_of_code_point(u32 code_point) const
    {
        return m_view.visit(
            [](Utf32View const&) { return 1; },
            [&](Utf16View const&) {
                if (code_point < 0x10000)
                    return 1;
                return 2;
            },
            [&](auto const&) {
                if (code_point <= 0x7f)
                    return 1;
                if (code_point <= 0x07ff)
                    return 2;
                if (code_point <= 0xffff)
                    return 3;
                return 4;
            });
    }

    RegexStringView typed_null_view()
    {
        auto view = m_view.visit(
            [&]<typename T>(T const&) {
                return RegexStringView { T {} };
            });
        view.set_unicode(unicode());
        return view;
    }

    RegexStringView construct_as_same(Span<u32> data, Optional<ByteString>& optional_string_storage, Utf16Data& optional_utf16_storage) const
    {
        auto view = m_view.visit(
            [&]<typename T>(T const&) {
                StringBuilder builder;
                for (auto ch : data)
                    builder.append(ch); // Note: The type conversion is intentional.
                optional_string_storage = builder.to_byte_string();
                return RegexStringView { T { *optional_string_storage } };
            },
            [&](Utf32View) {
                return RegexStringView { Utf32View { data.data(), data.size() } };
            },
            [&](Utf16View) {
                optional_utf16_storage = AK::utf32_to_utf16(Utf32View { data.data(), data.size() }).release_value_but_fixme_should_propagate_errors();
                return RegexStringView { Utf16View { optional_utf16_storage } };
            });

        view.set_unicode(unicode());
        return view;
    }

    Vector<RegexStringView> lines() const
    {
        return m_view.visit(
            [](StringView view) {
                auto views = view.lines(StringView::ConsiderCarriageReturn::No);
                Vector<RegexStringView> new_views;
                for (auto& view : views)
                    new_views.empend(view);
                return new_views;
            },
            [](Utf32View view) {
                if (view.is_empty())
                    return Vector<RegexStringView> { view };

                Vector<RegexStringView> views;
                u32 newline = '\n';
                while (!view.is_empty()) {
                    auto position = AK::memmem_optional(view.code_points(), view.length() * sizeof(u32), &newline, sizeof(u32));
                    if (!position.has_value())
                        break;
                    auto offset = position.value() / sizeof(u32);
                    views.empend(view.substring_view(0, offset));
                    view = view.substring_view(offset + 1, view.length() - offset - 1);
                }
                if (!view.is_empty())
                    views.empend(view);
                return views;
            },
            [](Utf16View view) {
                if (view.is_empty())
                    return Vector<RegexStringView> { view };

                Vector<RegexStringView> views;
                u16 newline = '\n';
                while (!view.is_empty()) {
                    auto position = AK::memmem_optional(view.data(), view.length_in_code_units() * sizeof(u16), &newline, sizeof(u16));
                    if (!position.has_value())
                        break;
                    auto offset = position.value() / sizeof(u16);
                    views.empend(view.substring_view(0, offset));
                    view = view.substring_view(offset + 1, view.length_in_code_units() - offset - 1);
                }
                if (!view.is_empty())
                    views.empend(view);
                return views;
            },
            [](Utf8View const& view) {
                if (view.is_empty())
                    return Vector<RegexStringView> { view };

                Vector<RegexStringView> views;
                auto it = view.begin();
                auto previous_newline_position_it = it;
                for (;;) {
                    if (*it == '\n') {
                        auto previous_offset = view.byte_offset_of(previous_newline_position_it);
                        auto new_offset = view.byte_offset_of(it);
                        auto slice = view.substring_view(previous_offset, new_offset - previous_offset);
                        views.empend(slice);
                        ++it;
                        previous_newline_position_it = it;
                    }
                    if (it.done())
                        break;
                    ++it;
                }
                if (it != previous_newline_position_it) {
                    auto previous_offset = view.byte_offset_of(previous_newline_position_it);
                    auto new_offset = view.byte_offset_of(it);
                    auto slice = view.substring_view(previous_offset, new_offset - previous_offset);
                    views.empend(slice);
                }
                return views;
            });
    }

    RegexStringView substring_view(size_t offset, size_t length) const
    {
        if (unicode()) {
            auto view = m_view.visit(
                [&](auto view) { return RegexStringView { view.substring_view(offset, length) }; },
                [&](Utf16View const& view) { return RegexStringView { view.unicode_substring_view(offset, length) }; },
                [&](Utf8View const& view) { return RegexStringView { view.unicode_substring_view(offset, length) }; });

            view.set_unicode(unicode());
            return view;
        }

        auto view = m_view.visit([&](auto view) { return RegexStringView { view.substring_view(offset, length) }; });
        view.set_unicode(unicode());
        return view;
    }

    ByteString to_byte_string() const
    {
        return m_view.visit(
            [](StringView view) { return view.to_byte_string(); },
            [](Utf16View view) { return view.to_byte_string(Utf16View::AllowInvalidCodeUnits::Yes).release_value_but_fixme_should_propagate_errors(); },
            [](auto& view) {
                StringBuilder builder;
                for (auto it = view.begin(); it != view.end(); ++it)
                    builder.append_code_point(*it);
                return builder.to_byte_string();
            });
    }

    ErrorOr<String> to_string() const
    {
        return m_view.visit(
            [](StringView view) { return String::from_utf8(view); },
            [](Utf16View view) { return view.to_utf8(Utf16View::AllowInvalidCodeUnits::Yes); },
            [](auto& view) -> ErrorOr<String> {
                StringBuilder builder;
                for (auto it = view.begin(); it != view.end(); ++it)
                    TRY(builder.try_append_code_point(*it));
                return builder.to_string();
            });
    }

    // Note: index must always be the code unit offset to return.
    u32 operator[](size_t index) const
    {
        return m_view.visit(
            [&](StringView view) -> u32 {
                auto ch = view[index];
                if constexpr (IsSigned<char>) {
                    if (ch < 0)
                        return 256u + ch;
                    return ch;
                }
            },
            [&](Utf32View const& view) -> u32 { return view[index]; },
            [&](Utf16View const& view) -> u32 { return view.code_point_at(index); },
            [&](Utf8View const& view) -> u32 {
                auto it = view.iterator_at_byte_offset(index);
                VERIFY(it != view.end());
                return *it;
            });
    }

    u32 code_unit_at(size_t code_unit_index) const
    {
        if (unicode())
            return operator[](code_unit_index);

        return m_view.visit(
            [&](StringView view) -> u32 {
                auto ch = view[code_unit_index];
                if constexpr (IsSigned<char>) {
                    if (ch < 0)
                        return 256u + ch;
                    return ch;
                }
            },
            [&](Utf32View const& view) -> u32 { return view[code_unit_index]; },
            [&](Utf16View const& view) -> u32 { return view.code_unit_at(code_unit_index); },
            [&](Utf8View const& view) -> u32 {
                auto it = view.iterator_at_byte_offset(code_unit_index);
                VERIFY(it != view.end());
                return *it;
            });
    }

    size_t code_unit_offset_of(size_t code_point_index) const
    {
        return m_view.visit(
            [&](StringView view) -> u32 {
                Utf8View utf8_view { view };
                return utf8_view.byte_offset_of(code_point_index);
            },
            [&](Utf32View const&) -> u32 { return code_point_index; },
            [&](Utf16View const& view) -> u32 {
                return view.code_unit_offset_of(code_point_index);
            },
            [&](Utf8View const& view) -> u32 {
                return view.byte_offset_of(code_point_index);
            });
    }

    bool operator==(char const* cstring) const
    {
        return m_view.visit(
            [&](Utf32View) { return to_byte_string() == cstring; },
            [&](Utf16View) { return to_byte_string() == cstring; },
            [&](Utf8View const& view) { return view.as_string() == cstring; },
            [&](StringView view) { return view == cstring; });
    }

    bool operator==(ByteString const& string) const
    {
        return m_view.visit(
            [&](Utf32View) { return to_byte_string() == string; },
            [&](Utf16View) { return to_byte_string() == string; },
            [&](Utf8View const& view) { return view.as_string() == string; },
            [&](StringView view) { return view == string; });
    }

    bool operator==(StringView string) const
    {
        return m_view.visit(
            [&](Utf32View) { return to_byte_string() == string; },
            [&](Utf16View) { return to_byte_string() == string; },
            [&](Utf8View const& view) { return view.as_string() == string; },
            [&](StringView view) { return view == string; });
    }

    bool operator==(Utf32View const& other) const
    {
        return m_view.visit(
            [&](Utf32View view) {
                return view.length() == other.length() && __builtin_memcmp(view.code_points(), other.code_points(), view.length() * sizeof(u32)) == 0;
            },
            [&](Utf16View) { return to_byte_string() == RegexStringView { other }.to_byte_string(); },
            [&](Utf8View const& view) { return view.as_string() == RegexStringView { other }.to_byte_string(); },
            [&](StringView view) { return view == RegexStringView { other }.to_byte_string(); });
    }

    bool operator==(Utf16View const& other) const
    {
        return m_view.visit(
            [&](Utf32View) { return to_byte_string() == RegexStringView { other }.to_byte_string(); },
            [&](Utf16View const& view) { return view == other; },
            [&](Utf8View const& view) { return view.as_string() == RegexStringView { other }.to_byte_string(); },
            [&](StringView view) { return view == RegexStringView { other }.to_byte_string(); });
    }

    bool operator==(Utf8View const& other) const
    {
        return m_view.visit(
            [&](Utf32View) { return to_byte_string() == other.as_string(); },
            [&](Utf16View) { return to_byte_string() == other.as_string(); },
            [&](Utf8View const& view) { return view.as_string() == other.as_string(); },
            [&](StringView view) { return other.as_string() == view; });
    }

    bool equals(RegexStringView other) const
    {
        return other.m_view.visit([this](auto const& view) { return operator==(view); });
    }

    bool equals_ignoring_case(RegexStringView other) const
    {
        // FIXME: Implement equals_ignoring_case() for unicode.
        return m_view.visit(
            [&](StringView view) {
                return other.m_view.visit(
                    [&](StringView other_view) { return view.equals_ignoring_ascii_case(other_view); },
                    [](auto&) -> bool { TODO(); });
            },
            [&](Utf16View view) {
                return other.m_view.visit(
                    [&](Utf16View other_view) { return view.equals_ignoring_case(other_view); },
                    [](auto&) -> bool { TODO(); });
            },
            [](auto&) -> bool { TODO(); });
    }

    bool starts_with(StringView str) const
    {
        return m_view.visit(
            [&](Utf32View) -> bool {
                TODO();
            },
            [&](Utf16View) -> bool {
                TODO();
            },
            [&](Utf8View const& view) { return view.as_string().starts_with(str); },
            [&](StringView view) { return view.starts_with(str); });
    }

    bool starts_with(Utf32View const& str) const
    {
        return m_view.visit(
            [&](Utf32View view) -> bool {
                if (str.length() > view.length())
                    return false;
                if (str.length() == view.length())
                    return operator==(str);
                for (size_t i = 0; i < str.length(); ++i) {
                    if (str.at(i) != view.at(i))
                        return false;
                }
                return true;
            },
            [&](Utf16View) -> bool { TODO(); },
            [&](Utf8View const& view) {
                auto it = view.begin();
                for (auto code_point : str) {
                    if (it.done())
                        return false;
                    if (code_point != *it)
                        return false;
                    ++it;
                }
                return true;
            },
            [&](StringView) -> bool { TODO(); });
    }

private:
    Variant<StringView, Utf8View, Utf16View, Utf32View> m_view { StringView {} };
    bool m_unicode { false };
};

class Match final {
private:
    Optional<FlyString> string;

public:
    Match() = default;
    ~Match() = default;

    Match(RegexStringView view_, size_t const line_, size_t const column_, size_t const global_offset_)
        : view(view_)
        , line(line_)
        , column(column_)
        , global_offset(global_offset_)
        , left_column(column_)
    {
    }

    Match(String string_, size_t const line_, size_t const column_, size_t const global_offset_)
        : string(move(string_))
        , view(string.value().bytes_as_string_view())
        , line(line_)
        , column(column_)
        , global_offset(global_offset_)
    {
    }

    Match(RegexStringView const view_, StringView capture_group_name_, size_t const line_, size_t const column_, size_t const global_offset_)
        : view(view_)
        , capture_group_name(MUST(FlyString::from_utf8(capture_group_name_)))
        , line(line_)
        , column(column_)
        , global_offset(global_offset_)
        , left_column(column_)
    {
    }

    void reset()
    {
        view = view.typed_null_view();
        capture_group_name.clear();
        line = 0;
        column = 0;
        global_offset = 0;
        left_column = 0;
    }

    RegexStringView view {};
    Optional<FlyString> capture_group_name {};
    size_t line { 0 };
    size_t column { 0 };
    size_t global_offset { 0 };

    // ugly, as not usable by user, but needed to prevent to create extra vectors that are
    // able to store the column when the left paren has been found
    size_t left_column { 0 };
};

struct MatchInput {
    RegexStringView view {};
    AllOptions regex_options {};
    size_t start_offset { 0 }; // For Stateful matches, saved and restored from Regex::start_offset.

    size_t match_index { 0 };
    size_t line { 0 };
    size_t column { 0 };

    size_t global_offset { 0 }; // For multiline matching, knowing the offset from start could be important

    mutable size_t fail_counter { 0 };
    mutable Vector<size_t> saved_positions;
    mutable Vector<size_t> saved_code_unit_positions;
    mutable Vector<size_t> saved_forks_since_last_save;
    mutable Optional<size_t> fork_to_replace;
};

struct MatchState {
    size_t string_position_before_match { 0 };
    size_t string_position { 0 };
    size_t string_position_in_code_units { 0 };
    size_t instruction_position { 0 };
    size_t fork_at_position { 0 };
    size_t forks_since_last_save { 0 };
    Optional<size_t> initiating_fork;
    COWVector<Match> matches;
    COWVector<Vector<Match>> capture_group_matches;
    COWVector<u64> repetition_marks;
    Vector<u64, 64> checkpoints;

    // For size_t in {0..100}, ips in {0..500} and repetitions in {0..30}, there are zero collisions.
    // For the full range, zero collisions were found in 8 million random samples.
    u64 u64_hash() const
    {
        u64 hash = 0xcbf29ce484222325;
        auto combine = [&hash](auto value) {
            hash ^= value + 0x9e3779b97f4a7c15 + (hash << 6) + (hash >> 2);
        };
        auto combine_vector = [&hash](auto const& vector) {
            for (auto& value : vector) {
                hash ^= value;
                hash *= 0x100000001b3;
            }
        };

        combine(string_position_before_match);
        combine(string_position);
        combine(string_position_in_code_units);
        combine(instruction_position);
        combine(fork_at_position);
        combine(initiating_fork.value_or(0) + initiating_fork.has_value());
        combine_vector(repetition_marks);
        combine_vector(checkpoints);

        return hash;
    }
};

}

using regex::RegexStringView;

template<>
struct AK::Formatter<regex::RegexStringView> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, regex::RegexStringView value)
    {
        auto string = value.to_byte_string();
        return Formatter<StringView>::format(builder, string);
    }
};
