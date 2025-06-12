/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AllOf.h>
#include <AK/AnyOf.h>
#include <AK/Array.h>
#include <AK/StringView.h>

#ifdef ENABLE_COMPILETIME_FORMAT_CHECK
namespace AK::Format::Detail {

// We have to define a local "purely constexpr" Array that doesn't lead back to us (via e.g. VERIFY)
template<typename T, size_t Size>
struct Array {
    constexpr static size_t size() { return Size; }
    constexpr T const& operator[](size_t index) const { return __data[index]; }
    constexpr T& operator[](size_t index) { return __data[index]; }
    using ConstIterator = SimpleIterator<Array const, T const>;
    using Iterator = SimpleIterator<Array, T>;

    constexpr ConstIterator begin() const { return ConstIterator::begin(*this); }
    constexpr Iterator begin() { return Iterator::begin(*this); }

    constexpr ConstIterator end() const { return ConstIterator::end(*this); }
    constexpr Iterator end() { return Iterator::end(*this); }

    T __data[Size];
};

template<size_t N>
consteval auto extract_used_argument_index(char const (&fmt)[N], size_t specifier_start_index, size_t specifier_end_index, size_t& next_implicit_argument_index)
{
    struct {
        size_t index_value { 0 };
        bool saw_explicit_index { false };
    } state;
    for (size_t i = specifier_start_index; i < specifier_end_index; ++i) {
        auto c = fmt[i];
        if (c > '9' || c < '0')
            break;

        state.index_value *= 10;
        state.index_value += c - '0';
        state.saw_explicit_index = true;
    }

    if (!state.saw_explicit_index)
        return next_implicit_argument_index++;

    return state.index_value;
}

// FIXME: We should rather parse these format strings at compile-time if possible.
template<size_t N>
consteval auto count_fmt_params(char const (&fmt)[N])
{
    struct {
        // FIXME: Switch to variable-sized storage whenever we can come up with one :)
        Array<size_t, 128> used_arguments { 0 };
        size_t total_used_argument_count { 0 };
        size_t next_implicit_argument_index { 0 };
        bool has_explicit_argument_references { false };

        size_t unclosed_braces { 0 };
        size_t extra_closed_braces { 0 };
        size_t nesting_level { 0 };

        Array<size_t, 4> last_format_specifier_start { 0 };
        size_t total_used_last_format_specifier_start_count { 0 };
    } result;

    for (size_t i = 0; i < N; ++i) {
        auto ch = fmt[i];
        switch (ch) {
        case '{':
            if (i + 1 < N && fmt[i + 1] == '{') {
                ++i;
                continue;
            }

            // Note: There's no compile-time throw, so we have to abuse a compile-time string to store errors.
            if (result.total_used_last_format_specifier_start_count >= result.last_format_specifier_start.size() - 1)
                compiletime_fail("Format-String Checker internal error: Format specifier nested too deep");

            result.last_format_specifier_start[result.total_used_last_format_specifier_start_count++] = i + 1;

            ++result.unclosed_braces;
            ++result.nesting_level;
            break;
        case '}':
            if (result.nesting_level == 0) {
                if (i + 1 < N && fmt[i + 1] == '}') {
                    ++i;
                    continue;
                }
            }
            if (result.unclosed_braces) {
                --result.nesting_level;
                --result.unclosed_braces;

                if (result.total_used_last_format_specifier_start_count == 0)
                    compiletime_fail("Format-String Checker internal error: Expected location information");

                auto const specifier_start_index = result.last_format_specifier_start[--result.total_used_last_format_specifier_start_count];

                if (result.total_used_argument_count >= result.used_arguments.size())
                    compiletime_fail("Format-String Checker internal error: Too many format arguments in format string");

                auto used_argument_index = extract_used_argument_index<N>(fmt, specifier_start_index, i, result.next_implicit_argument_index);
                if (used_argument_index + 1 != result.next_implicit_argument_index)
                    result.has_explicit_argument_references = true;
                result.used_arguments[result.total_used_argument_count++] = used_argument_index;

            } else {
                ++result.extra_closed_braces;
            }
            break;
        default:
            continue;
        }
    }
    return result;
}

}

#endif

namespace AK::Format::Detail {

template<typename... Args>
struct CheckedFormatString {
    template<size_t N>
    consteval CheckedFormatString(char const (&fmt)[N])
        : m_string { fmt, N - 1 }
    {
#ifdef ENABLE_COMPILETIME_FORMAT_CHECK
        check_format_parameter_consistency<N, sizeof...(Args)>(fmt);
#endif
    }

    template<typename T>
    CheckedFormatString(T const& unchecked_fmt)
    requires(requires(T t) { StringView { t }; })
        : m_string(unchecked_fmt)
    {
    }

    auto view() const { return m_string; }

private:
#ifdef ENABLE_COMPILETIME_FORMAT_CHECK
    template<size_t N, size_t param_count>
    consteval static bool check_format_parameter_consistency(char const (&fmt)[N])
    {
        auto check = count_fmt_params<N>(fmt);
        if (check.unclosed_braces != 0)
            compiletime_fail("Extra unclosed braces in format string");
        if (check.extra_closed_braces != 0)
            compiletime_fail("Extra closing braces in format string");

        {
            auto begin = check.used_arguments.begin();
            auto end = check.used_arguments.begin() + check.total_used_argument_count;
            auto has_all_referenced_arguments = !AK::any_of(begin, end, [](auto& entry) { return entry >= param_count; });
            if (!has_all_referenced_arguments)
                compiletime_fail("Format string references nonexistent parameter");
        }

        if (!check.has_explicit_argument_references && check.total_used_argument_count != param_count)
            compiletime_fail("Format string does not reference all passed parameters");

        // Ensure that no passed parameter is ignored or otherwise not referenced in the format
        // As this check is generally pretty expensive, try to avoid it where it cannot fail.
        // We will only do this check if the format string has explicit argument refs
        // otherwise, the check above covers this check too, as implicit refs
        // monotonically increase, and cannot have 'gaps'.
        if (check.has_explicit_argument_references) {
            auto all_parameters = iota_array<size_t, param_count>(0);
            constexpr auto contains = [](auto begin, auto end, auto entry) {
                for (; begin != end; begin++) {
                    if (*begin == entry)
                        return true;
                }

                return false;
            };
            auto references_all_arguments = AK::all_of(
                all_parameters,
                [&](auto& entry) {
                    return contains(
                        check.used_arguments.begin(),
                        check.used_arguments.begin() + check.total_used_argument_count,
                        entry);
                });
            if (!references_all_arguments)
                compiletime_fail("Format string does not reference all passed parameters");
        }

        return true;
    }
#endif

    StringView m_string;
};

}

namespace AK {

template<typename... Args>
using CheckedFormatString = Format::Detail::CheckedFormatString<IdentityType<Args>...>;

}
