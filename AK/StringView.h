/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Checked.h>
#include <AK/Concepts.h>
#include <AK/EnumBits.h>
#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <AK/StringHash.h>
#include <AK/StringUtils.h>

namespace AK {

class StringView {
public:
    ALWAYS_INLINE constexpr StringView() = default;

    ALWAYS_INLINE constexpr StringView(char const* characters, size_t length)
        : m_characters(characters)
        , m_length(length)
    {
        if (!is_constant_evaluated())
            VERIFY(!Checked<uintptr_t>::addition_would_overflow(reinterpret_cast<uintptr_t>(characters), length));
    }

    ALWAYS_INLINE StringView(unsigned char const* characters, size_t length)
        : m_characters(reinterpret_cast<char const*>(characters))
        , m_length(length)
    {
        VERIFY(!Checked<uintptr_t>::addition_would_overflow(reinterpret_cast<uintptr_t>(characters), length));
    }

    ALWAYS_INLINE StringView(ReadonlyBytes bytes)
        : m_characters(reinterpret_cast<char const*>(bytes.data()))
        , m_length(bytes.size())
    {
    }

    StringView(ByteBuffer const&);
    StringView(String const&);
    StringView(FlyString const&);
    StringView(ByteString const&);

    explicit StringView(ByteBuffer&&) = delete;
    explicit StringView(String&&) = delete;
    explicit StringView(FlyString&&) = delete;
    explicit StringView(ByteString&&) = delete;

    template<OneOf<String, FlyString, ByteString, ByteBuffer> StringType>
    StringView& operator=(StringType&&) = delete;

    [[nodiscard]] constexpr bool is_null() const { return m_characters == nullptr; }
    [[nodiscard]] constexpr bool is_empty() const { return m_length == 0; }

    [[nodiscard]] constexpr char const* characters_without_null_termination() const { return m_characters; }
    [[nodiscard]] constexpr size_t length() const { return m_length; }

    [[nodiscard]] ReadonlyBytes bytes() const { return { m_characters, m_length }; }

    constexpr char const& operator[](size_t index) const
    {
        if (!is_constant_evaluated())
            VERIFY(index < m_length);
        return m_characters[index];
    }

    using ConstIterator = SimpleIterator<StringView const, char const>;

    [[nodiscard]] constexpr ConstIterator begin() const { return ConstIterator::begin(*this); }
    [[nodiscard]] constexpr ConstIterator end() const { return ConstIterator::end(*this); }

    [[nodiscard]] constexpr unsigned hash() const
    {
        if (is_empty())
            return 0;
        return string_hash(characters_without_null_termination(), length());
    }

    [[nodiscard]] bool starts_with(StringView, CaseSensitivity = CaseSensitivity::CaseSensitive) const;
    [[nodiscard]] bool ends_with(StringView, CaseSensitivity = CaseSensitivity::CaseSensitive) const;
    [[nodiscard]] bool starts_with(char) const;
    [[nodiscard]] bool ends_with(char) const;
    [[nodiscard]] bool matches(StringView mask, CaseSensitivity = CaseSensitivity::CaseInsensitive) const;
    [[nodiscard]] bool matches(StringView mask, Vector<MaskSpan>&, CaseSensitivity = CaseSensitivity::CaseInsensitive) const;
    [[nodiscard]] bool contains(char) const;
    [[nodiscard]] bool contains(u32) const;
    [[nodiscard]] bool contains(StringView, CaseSensitivity = CaseSensitivity::CaseSensitive) const;
    [[nodiscard]] bool equals_ignoring_ascii_case(StringView) const;
    [[nodiscard]] bool is_ascii() const;

    [[nodiscard]] StringView trim(StringView characters, TrimMode mode = TrimMode::Both) const { return StringUtils::trim(*this, characters, mode); }
    [[nodiscard]] StringView trim_whitespace(TrimMode mode = TrimMode::Both) const { return StringUtils::trim_whitespace(*this, mode); }

    [[nodiscard]] String to_ascii_lowercase_string() const;
    [[nodiscard]] String to_ascii_uppercase_string() const;
    [[nodiscard]] String to_ascii_titlecase_string() const;

    [[nodiscard]] Optional<size_t> find(char needle, size_t start = 0) const { return StringUtils::find(*this, needle, start); }
    [[nodiscard]] Optional<size_t> find(StringView needle, size_t start = 0) const { return StringUtils::find(*this, needle, start); }
    [[nodiscard]] Optional<size_t> find_last(char needle) const { return StringUtils::find_last(*this, needle); }
    [[nodiscard]] Optional<size_t> find_last(StringView needle) const { return StringUtils::find_last(*this, needle); }
    [[nodiscard]] Optional<size_t> find_last_not(char needle) const { return StringUtils::find_last_not(*this, needle); }

    [[nodiscard]] Vector<size_t> find_all(StringView needle) const;

    using SearchDirection = StringUtils::SearchDirection;
    [[nodiscard]] Optional<size_t> find_any_of(StringView needles, SearchDirection direction = SearchDirection::Forward) const { return StringUtils::find_any_of(*this, needles, direction); }

    [[nodiscard]] constexpr StringView substring_view(size_t start, size_t length) const
    {
        if (!is_constant_evaluated())
            VERIFY(start + length <= m_length);
        return { m_characters + start, length };
    }

    [[nodiscard]] constexpr StringView substring_view(size_t start) const
    {
        if (!is_constant_evaluated())
            VERIFY(start <= length());
        return substring_view(start, length() - start);
    }

    [[nodiscard]] Vector<StringView> split_view(char, SplitBehavior = SplitBehavior::Nothing) const;
    [[nodiscard]] Vector<StringView> split_view(StringView, SplitBehavior = SplitBehavior::Nothing) const;

    [[nodiscard]] Vector<StringView> split_view_if(Function<bool(char)> const& predicate, SplitBehavior = SplitBehavior::Nothing) const;

    [[nodiscard]] StringView find_last_split_view(char separator) const
    {
        auto begin = find_last(separator);
        if (!begin.has_value())
            return *this;
        return substring_view(begin.release_value() + 1);
    }

    [[nodiscard]] StringView find_first_split_view(char separator) const
    {
        auto needle_begin = find(separator);
        if (!needle_begin.has_value())
            return *this;
        return substring_view(0, needle_begin.release_value());
    }

    template<typename Callback>
    auto for_each_split_view(char separator, SplitBehavior split_behavior, Callback callback) const
    {
        StringView seperator_view { &separator, 1 };
        return for_each_split_view(seperator_view, split_behavior, callback);
    }

    template<typename Callback>
    auto for_each_split_view(StringView separator, SplitBehavior split_behavior, Callback callback) const
    {
        VERIFY(!separator.is_empty());
        // FIXME: This can't go in the template header since declval won't allow the incomplete StringView type.
        using CallbackReturn = InvokeResult<Callback, StringView>;
        constexpr auto ReturnsErrorOr = FallibleFunction<Callback, StringView>;
        // FIXME: We might need a concept for this...
        constexpr auto ReturnsIterationDecision = []() -> bool {
            if constexpr (ReturnsErrorOr)
                return IsSame<typename CallbackReturn::ResultType, IterationDecision>;
            return IsSame<CallbackReturn, IterationDecision>;
        }();
        using ReturnType = Conditional<ReturnsErrorOr, ErrorOr<void>, void>;
        return [&]() -> ReturnType {
            if (is_empty())
                return ReturnType();

            StringView view { *this };
            auto maybe_separator_index = find(separator);
            bool keep_empty = has_flag(split_behavior, SplitBehavior::KeepEmpty);
            bool keep_separator = has_flag(split_behavior, SplitBehavior::KeepTrailingSeparator);
            while (maybe_separator_index.has_value()) {
                auto separator_index = maybe_separator_index.value();
                auto part_with_separator = view.substring_view(0, separator_index + separator.length());
                if (keep_empty || separator_index > 0) {
                    auto part = part_with_separator;
                    if (!keep_separator)
                        part = part_with_separator.substring_view(0, separator_index);
                    if constexpr (ReturnsErrorOr) {
                        if constexpr (ReturnsIterationDecision) {
                            if (TRY(callback(part)) == IterationDecision::Break)
                                return ReturnType();
                        } else {
                            TRY(callback(part));
                        }
                    } else {
                        if constexpr (ReturnsIterationDecision) {
                            if (callback(part) == IterationDecision::Break)
                                return ReturnType();
                        } else {
                            callback(part);
                        }
                    }
                }
                view = view.substring_view_starting_after_substring(part_with_separator);
                maybe_separator_index = view.find(separator);
            }
            if (keep_empty || !view.is_empty()) {
                if constexpr (ReturnsErrorOr)
                    TRY(callback(view));
                else
                    callback(view);
            }

            return ReturnType();
        }();
    }

    // Create a Vector of StringViews split by line endings. As of CommonMark
    // 0.29, the spec defines a line ending as "a newline (U+000A), a carriage
    // return (U+000D) not followed by a newline, or a carriage return and a
    // following newline.".
    enum class ConsiderCarriageReturn {
        No,
        Yes,
    };
    [[nodiscard]] Vector<StringView> lines(ConsiderCarriageReturn = ConsiderCarriageReturn::Yes) const;
    [[nodiscard]] size_t count_lines(ConsiderCarriageReturn = ConsiderCarriageReturn::Yes) const;

    // Create a new substring view of this string view, starting either at the beginning of
    // the given substring view, or after its end, and continuing until the end of this string
    // view (that is, for the remaining part of its length). For example,
    //
    //    StringView str { "foobar" };
    //    StringView substr = str.substring_view(1, 2);  // "oo"
    //    StringView substr_from = str.substring_view_starting_from_substring(subst);  // "oobar"
    //    StringView substr_after = str.substring_view_starting_after_substring(subst);  // "bar"
    //
    // Note that this only works if the string view passed as an argument is indeed a substring
    // view of this string view, such as one created by substring_view() and split_view(). It
    // does not work for arbitrary strings; for example declaring substr in the example above as
    //
    //     StringView substr { "oo" };
    //
    // would not work.
    [[nodiscard]] StringView substring_view_starting_from_substring(StringView substring) const;
    [[nodiscard]] StringView substring_view_starting_after_substring(StringView substring) const;

    [[nodiscard]] bool copy_characters_to_buffer(char* buffer, size_t buffer_size) const;

    constexpr bool operator==(char const* cstring) const
    {
        if (is_null())
            return cstring == nullptr;
        if (!cstring)
            return false;
        // NOTE: `m_characters` is not guaranteed to be null-terminated, but `cstring` is.
        char const* cp = cstring;
        for (size_t i = 0; i < m_length; ++i) {
            if (*cp == '\0')
                return false;
            if (m_characters[i] != *(cp++))
                return false;
        }
        return *cp == '\0';
    }

    constexpr bool operator==(char const c) const
    {
        return m_length == 1 && *m_characters == c;
    }

    bool operator==(ByteString const&) const;

    [[nodiscard]] constexpr int compare(StringView other) const
    {
        if (m_length == 0 && other.m_length == 0)
            return 0;

        if (m_characters == nullptr)
            return other.m_characters ? -1 : 0;

        if (other.m_characters == nullptr)
            return 1;

        size_t rlen = min(m_length, other.m_length);
        int c = __builtin_memcmp(m_characters, other.m_characters, rlen);
        if (c == 0) {
            if (length() < other.length())
                return -1;
            if (length() == other.length())
                return 0;
            return 1;
        }
        return c;
    }

    constexpr bool operator==(StringView other) const
    {
        return length() == other.length() && compare(other) == 0;
    }

    constexpr bool operator!=(StringView other) const
    {
        return length() != other.length() || compare(other) != 0;
    }

    constexpr bool operator<(StringView other) const { return compare(other) < 0; }
    constexpr bool operator<=(StringView other) const { return compare(other) <= 0; }
    constexpr bool operator>(StringView other) const { return compare(other) > 0; }
    constexpr bool operator>=(StringView other) const { return compare(other) >= 0; }

    [[nodiscard]] ByteString to_byte_string() const;

    [[nodiscard]] bool is_whitespace() const
    {
        return StringUtils::is_whitespace(*this);
    }

    [[nodiscard]] ByteString replace(StringView needle, StringView replacement, ReplaceMode) const;

    [[nodiscard]] size_t count(StringView needle) const
    {
        return StringUtils::count(*this, needle);
    }

    [[nodiscard]] size_t count(char needle) const
    {
        return StringUtils::count(*this, needle);
    }

    template<typename... Ts>
    [[nodiscard]] ALWAYS_INLINE constexpr bool is_one_of(Ts&&... strings) const
    {
        return (... || this->operator==(forward<Ts>(strings)));
    }

    template<typename... Ts>
    [[nodiscard]] ALWAYS_INLINE constexpr bool is_one_of_ignoring_ascii_case(Ts&&... strings) const
    {
        return (... ||
                [this, &strings]() -> bool {
            if constexpr (requires(Ts a) { a.view()->StringView; })
                return this->equals_ignoring_ascii_case(forward<Ts>(strings.view()));
            else
                return this->equals_ignoring_ascii_case(forward<Ts>(strings));
        }());
    }

    template<Arithmetic T>
    Optional<T> to_number(TrimWhitespace trim_whitespace = TrimWhitespace::Yes) const
    {
        if constexpr (IsFloatingPoint<T>)
            return StringUtils::convert_to_floating_point<T>(*this, trim_whitespace);
        if constexpr (IsSigned<T>)
            return StringUtils::convert_to_int<T>(*this, trim_whitespace);
        else
            return StringUtils::convert_to_uint<T>(*this, trim_whitespace);
    }

private:
    friend class ByteString;
    char const* m_characters { nullptr };
    size_t m_length { 0 };
};

template<>
struct Traits<StringView> : public DefaultTraits<StringView> {
    using PeekType = StringView;
    using ConstPeekType = StringView;
    static unsigned hash(StringView s) { return s.hash(); }
};

struct CaseInsensitiveASCIIStringViewTraits : public Traits<StringView> {
    static unsigned hash(StringView s)
    {
        if (s.is_empty())
            return 0;
        return case_insensitive_string_hash(s.characters_without_null_termination(), s.length());
    }
    static bool equals(StringView const& a, StringView const& b) { return a.equals_ignoring_ascii_case(b); }
};

}

// FIXME: Remove this when clang on BSD distributions fully support consteval (specifically in the context of default parameter initialization).
//        Note that this is fixed in clang-15, but is not yet picked up by all downstream distributions.
//        See: https://github.com/llvm/llvm-project/issues/48230
//        Additionally, oss-fuzz currently ships an llvm-project commit that is a pre-release of 15.0.0.
//        See: https://github.com/google/oss-fuzz/issues/9989
//        Android currently doesn't ship clang-15 in any NDK
#if defined(AK_OS_BSD_GENERIC) || defined(OSS_FUZZ) || defined(AK_OS_ANDROID)
#    define AK_STRING_VIEW_LITERAL_CONSTEVAL constexpr
#else
#    define AK_STRING_VIEW_LITERAL_CONSTEVAL consteval
#endif

[[nodiscard]] ALWAYS_INLINE AK_STRING_VIEW_LITERAL_CONSTEVAL AK::StringView operator""sv(char const* cstring, size_t length)
{
    return AK::StringView(cstring, length);
}

#if USING_AK_GLOBALLY
using AK::CaseInsensitiveASCIIStringViewTraits;
using AK::StringView;
#endif
