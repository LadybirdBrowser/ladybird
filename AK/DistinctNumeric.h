/*
 * Copyright (c) 2020, Ben Wiederhake <BenWiederhake.GitHub@gmx.de>
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/Traits.h>
#include <AK/Types.h>

namespace AK {

/**
 * This implements a "distinct" numeric type that is intentionally incompatible
 * to other incantations. The intention is that each "distinct" type that you
 * want simply gets different values for `fn_length` and `line`. The macros
 * `TYPEDEF_DISTINCT_NUMERIC_*()` at the bottom of `DistinctNumeric.h`.
 *
 * The tags in `DistinctNumericFeature` simply split up the space of operators into 6 simple categories:
 * - No matter the values of these, `DistinctNumeric` always implements `==` and `!=`.
 * - If `Arithmetic` is present, then `a+b`, `a-b`, `+a`, `-a`, `a*b`, `a/b`, `a%b`, and the respective `a_=b` versions are implemented.
 * - If `CastToBool` is present, then `!a`, `a&&b`, and `a||b` are implemented (but not `operator bool()`, because of overzealous integer promotion rules).
 * - If `Comparison` is present, then `a>b`, `a<b`, `a>=b`, and `a<=b` are implemented via operator<=>
 * - If `Flags` is present, then `~a`, `a&b`, `a|b`, `a^b`, `a&=b`, `a|=b`, and `a^=b` are implemented.
 * - If `Increment` is present, then `++a`, `a++`, `--a`, and `a--` are implemented.
 * - If `Shift` is present, then `a<<b`, `a>>b`, `a<<=b`, `a>>=b` are implemented.
 * The semantics are always those of the underlying basic type `T`.
 *
 * These can be combined arbitrarily. Want a numeric type that supports `++a`
 * and `a >> b` but not `a > b`? Sure thing, just set
 * `Increment, Comparison, Shift` and you're done!
 * Furthermore, some of these overloads make more sense with specific types, like `a&&b` which should be able to operate
 *
 * I intentionally decided against overloading `&a` because these shall remain
 * numeric types.
 *
 * The are many operators that do not work on `int`, so I left them out:
 * `a[b]`, `*a`, `a->b`, `a.b`, `a->*b`, `a.*b`.
 *
 * There are many more operators that do not make sense for numerical types,
 * or cannot be overloaded in the first place. Naturally, they are not implemented.
 */

namespace DistinctNumericFeature {

enum Arithmetic { };
enum CastToBool { };
enum CastToUnderlying { };
enum Comparison { };
enum Flags { };
enum Increment { };
enum Shift { };

};

template<typename T, typename X, typename... Opts>
class DistinctNumeric {
    using Self = DistinctNumeric<T, X, Opts...>;

    struct Option {
        template<typename K, typename... Os>
        consteval Option(K option, Os... other_options)
            : Option(other_options...)
        {
            set(option);
        }

        consteval Option() { }

        constexpr void set(DistinctNumericFeature::Arithmetic const&) { arithmetic = true; }
        constexpr void set(DistinctNumericFeature::CastToBool const&) { cast_to_bool = true; }
        constexpr void set(DistinctNumericFeature::CastToUnderlying const&) { cast_to_underlying = true; }
        constexpr void set(DistinctNumericFeature::Comparison const&) { comparisons = true; }
        constexpr void set(DistinctNumericFeature::Flags const&) { flags = true; }
        constexpr void set(DistinctNumericFeature::Increment const&) { increment = true; }
        constexpr void set(DistinctNumericFeature::Shift const&) { shift = true; }

        bool arithmetic { false };
        bool cast_to_bool { false };
        bool cast_to_underlying { false };
        bool comparisons { false };
        bool flags { false };
        bool increment { false };
        bool shift { false };
    };

    constexpr static Option options { Opts()... };

public:
    using Type = T;

    constexpr DistinctNumeric() = default;

    constexpr DistinctNumeric(T value)
        : m_value { value }
    {
    }

    constexpr T const& value() const { return m_value; }
    constexpr T& value() { return m_value; }

    // Always implemented: identity.
    constexpr bool operator==(Self const& other) const
    {
        return this->m_value == other.m_value;
    }

    // Only implemented when `CastToUnderlying` is true:
    constexpr explicit operator T() const
    {
        static_assert(options.cast_to_underlying, "Cast to underlying type is only available for DistinctNumeric types with 'CastToUnderlying'.");
        return value();
    }

    // Only implemented when `Increment` is true:
    constexpr Self& operator++()
    {
        static_assert(options.increment, "'++a' is only available for DistinctNumeric types with 'Increment'.");
        this->m_value += 1;
        return *this;
    }
    constexpr Self operator++(int)
    {
        static_assert(options.increment, "'a++' is only available for DistinctNumeric types with 'Increment'.");
        Self ret = this->m_value;
        this->m_value += 1;
        return ret;
    }
    constexpr Self& operator--()
    {
        static_assert(options.increment, "'--a' is only available for DistinctNumeric types with 'Increment'.");
        this->m_value -= 1;
        return *this;
    }
    constexpr Self operator--(int)
    {
        static_assert(options.increment, "'a--' is only available for DistinctNumeric types with 'Increment'.");
        Self ret = this->m_value;
        this->m_value -= 1;
        return ret;
    }

    // Only implemented when `Comparison` is true:
    constexpr int operator<=>(Self const& other) const
    {
        static_assert(options.comparisons, "'a<=>b' is only available for DistinctNumeric types with 'Comparison'.");
        return this->m_value > other.m_value ? 1 : this->m_value < other.m_value ? -1
                                                                                 : 0;
    }

    // Only implemented when `CastToBool` is true:
    constexpr bool operator!() const
    {
        static_assert(options.cast_to_bool, "'!a' is only available for DistinctNumeric types with 'CastToBool'.");
        return !this->m_value;
    }
    // Intentionally don't define `operator bool() const` here. C++ is a bit
    // overzealous, and whenever there would be a type error, C++ instead tries
    // to convert to a common int-ish type first. `bool` is int-ish, so
    // `operator bool() const` would defy the entire point of this class.

    // Only implemented when `Flags` is true:
    constexpr Self operator~() const
    {
        static_assert(options.flags, "'~a' is only available for DistinctNumeric types with 'Flags'.");
        return ~this->m_value;
    }
    constexpr Self operator&(Self const& other) const
    {
        static_assert(options.flags, "'a&b' is only available for DistinctNumeric types with 'Flags'.");
        return this->m_value & other.m_value;
    }
    constexpr Self operator|(Self const& other) const
    {
        static_assert(options.flags, "'a|b' is only available for DistinctNumeric types with 'Flags'.");
        return this->m_value | other.m_value;
    }
    constexpr Self operator^(Self const& other) const
    {
        static_assert(options.flags, "'a^b' is only available for DistinctNumeric types with 'Flags'.");
        return this->m_value ^ other.m_value;
    }
    constexpr Self& operator&=(Self const& other)
    {
        static_assert(options.flags, "'a&=b' is only available for DistinctNumeric types with 'Flags'.");
        this->m_value &= other.m_value;
        return *this;
    }
    constexpr Self& operator|=(Self const& other)
    {
        static_assert(options.flags, "'a|=b' is only available for DistinctNumeric types with 'Flags'.");
        this->m_value |= other.m_value;
        return *this;
    }
    constexpr Self& operator^=(Self const& other)
    {
        static_assert(options.flags, "'a^=b' is only available for DistinctNumeric types with 'Flags'.");
        this->m_value ^= other.m_value;
        return *this;
    }

    // Only implemented when `Shift` is true:
    // TODO: Should this take `int` instead?
    constexpr Self operator<<(Self const& other) const
    {
        static_assert(options.shift, "'a<<b' is only available for DistinctNumeric types with 'Shift'.");
        return this->m_value << other.m_value;
    }
    constexpr Self operator>>(Self const& other) const
    {
        static_assert(options.shift, "'a>>b' is only available for DistinctNumeric types with 'Shift'.");
        return this->m_value >> other.m_value;
    }
    constexpr Self& operator<<=(Self const& other)
    {
        static_assert(options.shift, "'a<<=b' is only available for DistinctNumeric types with 'Shift'.");
        this->m_value <<= other.m_value;
        return *this;
    }
    constexpr Self& operator>>=(Self const& other)
    {
        static_assert(options.shift, "'a>>=b' is only available for DistinctNumeric types with 'Shift'.");
        this->m_value >>= other.m_value;
        return *this;
    }

    // Only implemented when `Arithmetic` is true:
    constexpr Self operator+(Self const& other) const
    {
        static_assert(options.arithmetic, "'a+b' is only available for DistinctNumeric types with 'Arithmetic'.");
        return this->m_value + other.m_value;
    }
    constexpr Self operator-(Self const& other) const
    {
        static_assert(options.arithmetic, "'a-b' is only available for DistinctNumeric types with 'Arithmetic'.");
        return this->m_value - other.m_value;
    }
    constexpr Self operator+() const
    {
        static_assert(options.arithmetic, "'+a' is only available for DistinctNumeric types with 'Arithmetic'.");
        return +this->m_value;
    }
    constexpr Self operator-() const
    {
        static_assert(options.arithmetic, "'-a' is only available for DistinctNumeric types with 'Arithmetic'.");
        return -this->m_value;
    }
    constexpr Self operator*(Self const& other) const
    {
        static_assert(options.arithmetic, "'a*b' is only available for DistinctNumeric types with 'Arithmetic'.");
        return this->m_value * other.m_value;
    }
    constexpr Self operator/(Self const& other) const
    {
        static_assert(options.arithmetic, "'a/b' is only available for DistinctNumeric types with 'Arithmetic'.");
        return this->m_value / other.m_value;
    }
    constexpr Self operator%(Self const& other) const
    {
        static_assert(options.arithmetic, "'a%b' is only available for DistinctNumeric types with 'Arithmetic'.");
        return this->m_value % other.m_value;
    }
    constexpr Self& operator+=(Self const& other)
    {
        static_assert(options.arithmetic, "'a+=b' is only available for DistinctNumeric types with 'Arithmetic'.");
        this->m_value += other.m_value;
        return *this;
    }
    constexpr Self& operator-=(Self const& other)
    {
        static_assert(options.arithmetic, "'a-=b' is only available for DistinctNumeric types with 'Arithmetic'.");
        this->m_value -= other.m_value;
        return *this;
    }
    constexpr Self& operator*=(Self const& other)
    {
        static_assert(options.arithmetic, "'a*=b' is only available for DistinctNumeric types with 'Arithmetic'.");
        this->m_value *= other.m_value;
        return *this;
    }
    constexpr Self& operator/=(Self const& other)
    {
        static_assert(options.arithmetic, "'a/=b' is only available for DistinctNumeric types with 'Arithmetic'.");
        this->m_value /= other.m_value;
        return *this;
    }
    constexpr Self& operator%=(Self const& other)
    {
        static_assert(options.arithmetic, "'a%=b' is only available for DistinctNumeric types with 'Arithmetic'.");
        this->m_value %= other.m_value;
        return *this;
    }

private:
    T m_value {};
};

template<typename T, typename X, typename... Opts>
struct Formatter<DistinctNumeric<T, X, Opts...>> : Formatter<T> {
    ErrorOr<void> format(FormatBuilder& builder, DistinctNumeric<T, X, Opts...> value)
    {
        return Formatter<T>::format(builder, value.value());
    }
};

}

#define AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(T, NAME, ...)                                       \
    struct NAME##_decl {                                                                        \
        using Arithmetic [[maybe_unused]] = AK::DistinctNumericFeature::Arithmetic;             \
        using CastToBool [[maybe_unused]] = AK::DistinctNumericFeature::CastToBool;             \
        using CastToUnderlying [[maybe_unused]] = AK::DistinctNumericFeature::CastToUnderlying; \
        using Comparison [[maybe_unused]] = AK::DistinctNumericFeature::Comparison;             \
        using Flags [[maybe_unused]] = AK::DistinctNumericFeature::Flags;                       \
        using Increment [[maybe_unused]] = AK::DistinctNumericFeature::Increment;               \
        using Shift [[maybe_unused]] = AK::DistinctNumericFeature::Shift;                       \
        using NAME [[maybe_unused]] = DistinctNumeric<T, struct __##NAME##_tag, ##__VA_ARGS__>; \
    };                                                                                          \
    using NAME = typename NAME##_decl::NAME;

#define AK_TYPEDEF_DISTINCT_ORDERED_ID(T, NAME) AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(T, NAME, Comparison, CastToBool)
// TODO: Further type aliases?

#define AK_MAKE_DISTINCT_NUMERIC_COMPARABLE_TO_ENUM(DN, E) \
    constexpr bool operator==(DN n, E e) { return n.value() == to_underlying(e); }

template<typename T, typename X, typename... Opts>
struct Traits<AK::DistinctNumeric<T, X, Opts...>> : public DefaultTraits<AK::DistinctNumeric<T, X, Opts...>> {
    static constexpr bool is_trivial() { return true; }
    static constexpr auto hash(DistinctNumeric<T, X, Opts...> const& d) { return Traits<T>::hash(d.value()); }
};

#if USING_AK_GLOBALLY
using AK::DistinctNumeric;
#endif
