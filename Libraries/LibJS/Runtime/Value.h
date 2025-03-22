/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/BitCast.h>
#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/Result.h>
#include <AK/SourceLocation.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <LibGC/NanBoxedValue.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <math.h>

namespace JS {

// 2 ** 53 - 1
static constexpr double MAX_ARRAY_LIKE_INDEX = 9007199254740991.0;
// Unique bit representation of negative zero (only sign bit set)
static constexpr u64 NEGATIVE_ZERO_BITS = ((u64)1 << 63);

// This leaves us 3 bits to tag the type of pointer:
static constexpr u64 OBJECT_TAG = 0b001 | GC::IS_CELL_BIT;
static constexpr u64 STRING_TAG = 0b010 | GC::IS_CELL_BIT;
static constexpr u64 SYMBOL_TAG = 0b011 | GC::IS_CELL_BIT;
static constexpr u64 ACCESSOR_TAG = 0b100 | GC::IS_CELL_BIT;
static constexpr u64 BIGINT_TAG = 0b101 | GC::IS_CELL_BIT;

// We can then by extracting the top 13 bits quickly check if a Value is
// pointer backed.
static_assert((OBJECT_TAG & GC::IS_CELL_PATTERN) == GC::IS_CELL_PATTERN);
static_assert((STRING_TAG & GC::IS_CELL_PATTERN) == GC::IS_CELL_PATTERN);
static_assert((GC::CANON_NAN_BITS & GC::IS_CELL_PATTERN) != GC::IS_CELL_PATTERN);
static_assert((GC::NEGATIVE_INFINITY_BITS & GC::IS_CELL_PATTERN) != GC::IS_CELL_PATTERN);

// Then for the non pointer backed types we don't set the sign bit and use the
// three lower bits for tagging as well.
static constexpr u64 UNDEFINED_TAG = 0b110 | GC::BASE_TAG;
static constexpr u64 NULL_TAG = 0b111 | GC::BASE_TAG;
static constexpr u64 BOOLEAN_TAG = 0b001 | GC::BASE_TAG;
static constexpr u64 INT32_TAG = 0b010 | GC::BASE_TAG;
static constexpr u64 EMPTY_TAG = 0b011 | GC::BASE_TAG;
// Notice how only undefined and null have the top bit set, this mean we can
// quickly check for nullish values by checking if the top and bottom bits are set
// but the middle one isn't.
static constexpr u64 IS_NULLISH_EXTRACT_PATTERN = 0xFFFEULL;
static constexpr u64 IS_NULLISH_PATTERN = 0x7FFEULL;
static_assert((UNDEFINED_TAG & IS_NULLISH_EXTRACT_PATTERN) == IS_NULLISH_PATTERN);
static_assert((NULL_TAG & IS_NULLISH_EXTRACT_PATTERN) == IS_NULLISH_PATTERN);
static_assert((BOOLEAN_TAG & IS_NULLISH_EXTRACT_PATTERN) != IS_NULLISH_PATTERN);
static_assert((INT32_TAG & IS_NULLISH_EXTRACT_PATTERN) != IS_NULLISH_PATTERN);
static_assert((EMPTY_TAG & IS_NULLISH_EXTRACT_PATTERN) != IS_NULLISH_PATTERN);
// We also have the empty tag to represent array holes however since empty
// values are not valid anywhere else we can use this "value" to our advantage
// in Optional<Value> to represent the empty optional.

static constexpr u64 SHIFTED_BOOLEAN_TAG = BOOLEAN_TAG << GC::TAG_SHIFT;
static constexpr u64 SHIFTED_INT32_TAG = INT32_TAG << GC::TAG_SHIFT;

// Summary:
// To pack all the different value in to doubles we use the following schema:
// s = sign, e = exponent, m = mantissa
// The top part is the tag and the bottom the payload.
// 0bseeeeeeeeeeemmmm mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// 0b0111111111111000 0... is the only real NaN
// 0b1111111111111xxx yyy... xxx = pointer type, yyy = pointer value
// 0b0111111111111xxx yyy... xxx = non-pointer type, yyy = value or 0 if just type

// Future expansion: We are not fully utilizing all the possible bit patterns
// yet, these choices were made to make it easy to implement and understand.
// We can for example drop the always 1 top bit of the mantissa expanding our
// options from 8 tags to 15 but since we currently only use 5 for both sign bits
// this is not needed.

class Value : public GC::NanBoxedValue {
public:
    enum class PreferredType {
        Default,
        String,
        Number,
    };

    [[nodiscard]] u16 tag() const { return m_value.tag; }

    bool is_empty() const { return m_value.tag == EMPTY_TAG; }
    bool is_undefined() const { return m_value.tag == UNDEFINED_TAG; }
    bool is_null() const { return m_value.tag == NULL_TAG; }
    bool is_number() const { return is_double() || is_int32(); }
    bool is_string() const { return m_value.tag == STRING_TAG; }
    bool is_object() const { return m_value.tag == OBJECT_TAG; }
    bool is_boolean() const { return m_value.tag == BOOLEAN_TAG; }
    bool is_symbol() const { return m_value.tag == SYMBOL_TAG; }
    bool is_accessor() const { return m_value.tag == ACCESSOR_TAG; }
    bool is_bigint() const { return m_value.tag == BIGINT_TAG; }
    bool is_nullish() const { return (m_value.tag & IS_NULLISH_EXTRACT_PATTERN) == IS_NULLISH_PATTERN; }
    ThrowCompletionOr<bool> is_array(VM&) const;
    bool is_function() const;
    bool is_constructor() const;
    bool is_error() const;
    ThrowCompletionOr<bool> is_regexp(VM&) const;

    bool is_infinity() const
    {
        static_assert(GC::NEGATIVE_INFINITY_BITS == (0x1ULL << 63 | GC::POSITIVE_INFINITY_BITS));
        return (0x1ULL << 63 | m_value.encoded) == GC::NEGATIVE_INFINITY_BITS;
    }

    bool is_positive_infinity() const
    {
        return m_value.encoded == GC::POSITIVE_INFINITY_BITS;
    }

    bool is_negative_infinity() const
    {
        return m_value.encoded == GC::NEGATIVE_INFINITY_BITS;
    }

    bool is_positive_zero() const
    {
        return m_value.encoded == 0 || (is_int32() && as_i32() == 0);
    }

    bool is_negative_zero() const
    {
        return m_value.encoded == NEGATIVE_ZERO_BITS;
    }

    bool is_integral_number() const
    {
        if (is_int32())
            return true;
        return is_finite_number() && trunc(as_double()) == as_double();
    }

    bool is_finite_number() const
    {
        if (!is_number())
            return false;
        if (is_int32())
            return true;
        return !is_nan() && !is_infinity();
    }

    Value()
        : Value(EMPTY_TAG << GC::TAG_SHIFT, (u64)0)
    {
    }

    template<typename T>
    requires(IsSameIgnoringCV<T, bool>) explicit Value(T value)
        : Value(BOOLEAN_TAG << GC::TAG_SHIFT, (u64)value)
    {
    }

    explicit Value(double value)
    {
        bool is_negative_zero = bit_cast<u64>(value) == NEGATIVE_ZERO_BITS;
        if (value >= NumericLimits<i32>::min() && value <= NumericLimits<i32>::max() && trunc(value) == value && !is_negative_zero) {
            ASSERT(!(SHIFTED_INT32_TAG & (static_cast<i32>(value) & 0xFFFFFFFFul)));
            m_value.encoded = SHIFTED_INT32_TAG | (static_cast<i32>(value) & 0xFFFFFFFFul);
        } else {
            if (isnan(value)) [[unlikely]]
                m_value.encoded = GC::CANON_NAN_BITS;
            else
                m_value.as_double = value;
        }
    }

    explicit Value(f16 value)
        : Value(static_cast<double>(value))
    {
    }

    // NOTE: A couple of integral types are excluded here:
    // - i32 has its own dedicated Value constructor
    // - i64 cannot safely be cast to a double
    // - bool isn't a number type and has its own dedicated Value constructor
    template<typename T>
    requires(IsIntegral<T> && !IsSameIgnoringCV<T, i32> && !IsSameIgnoringCV<T, i64> && !IsSameIgnoringCV<T, bool>) explicit Value(T value)
    {
        if (value > NumericLimits<i32>::max()) {
            m_value.as_double = static_cast<double>(value);
        } else {
            ASSERT(!(SHIFTED_INT32_TAG & (static_cast<i32>(value) & 0xFFFFFFFFul)));
            m_value.encoded = SHIFTED_INT32_TAG | (static_cast<i32>(value) & 0xFFFFFFFFul);
        }
    }

    explicit Value(unsigned value)
    {
        if (value > NumericLimits<i32>::max()) {
            m_value.as_double = static_cast<double>(value);
        } else {
            ASSERT(!(SHIFTED_INT32_TAG & (static_cast<i32>(value) & 0xFFFFFFFFul)));
            m_value.encoded = SHIFTED_INT32_TAG | (static_cast<i32>(value) & 0xFFFFFFFFul);
        }
    }

    explicit Value(i32 value)
        : Value(SHIFTED_INT32_TAG, (u32)value)
    {
    }

    Value(Cell const* cell)
        : Value(GC::IS_CELL_BIT << GC::TAG_SHIFT, reinterpret_cast<void const*>(cell))
    {
    }

    Value(Object const* object)
        : Value(OBJECT_TAG << GC::TAG_SHIFT, reinterpret_cast<void const*>(object))
    {
    }

    Value(PrimitiveString const* string)
        : Value(STRING_TAG << GC::TAG_SHIFT, reinterpret_cast<void const*>(string))
    {
    }

    Value(Symbol const* symbol)
        : Value(SYMBOL_TAG << GC::TAG_SHIFT, reinterpret_cast<void const*>(symbol))
    {
    }

    Value(Accessor const* accessor)
        : Value(ACCESSOR_TAG << GC::TAG_SHIFT, reinterpret_cast<void const*>(accessor))
    {
    }

    Value(BigInt const* bigint)
        : Value(BIGINT_TAG << GC::TAG_SHIFT, reinterpret_cast<void const*>(bigint))
    {
    }

    template<typename T>
    Value(GC::Ptr<T> ptr)
        : Value(ptr.ptr())
    {
    }

    template<typename T>
    Value(GC::Ref<T> ptr)
        : Value(ptr.ptr())
    {
    }

    template<typename T>
    Value(GC::Root<T> const& ptr)
        : Value(ptr.ptr())
    {
    }

    Cell& as_cell()
    {
        VERIFY(is_cell());
        return *extract_pointer<Cell>();
    }

    Cell& as_cell() const
    {
        VERIFY(is_cell());
        return *extract_pointer<Cell>();
    }

    double as_double() const
    {
        VERIFY(is_number());
        if (is_int32())
            return as_i32();
        return m_value.as_double;
    }

    bool as_bool() const
    {
        VERIFY(is_boolean());
        return static_cast<bool>(m_value.encoded & 0x1);
    }

    Object& as_object()
    {
        VERIFY(is_object());
        return *extract_pointer<Object>();
    }

    Object const& as_object() const
    {
        VERIFY(is_object());
        return *extract_pointer<Object>();
    }

    PrimitiveString& as_string()
    {
        VERIFY(is_string());
        return *extract_pointer<PrimitiveString>();
    }

    PrimitiveString const& as_string() const
    {
        VERIFY(is_string());
        return *extract_pointer<PrimitiveString>();
    }

    Symbol& as_symbol()
    {
        VERIFY(is_symbol());
        return *extract_pointer<Symbol>();
    }

    Symbol const& as_symbol() const
    {
        VERIFY(is_symbol());
        return *extract_pointer<Symbol>();
    }

    Accessor& as_accessor()
    {
        VERIFY(is_accessor());
        return *extract_pointer<Accessor>();
    }

    BigInt const& as_bigint() const
    {
        VERIFY(is_bigint());
        return *extract_pointer<BigInt>();
    }

    BigInt& as_bigint()
    {
        VERIFY(is_bigint());
        return *extract_pointer<BigInt>();
    }

    Array& as_array();
    FunctionObject& as_function();
    FunctionObject const& as_function() const;

    u64 encoded() const { return m_value.encoded; }

    ThrowCompletionOr<String> to_string(VM&) const;
    ThrowCompletionOr<ByteString> to_byte_string(VM&) const;
    ThrowCompletionOr<Utf16String> to_utf16_string(VM&) const;
    ThrowCompletionOr<String> to_well_formed_string(VM&) const;
    ThrowCompletionOr<GC::Ref<PrimitiveString>> to_primitive_string(VM&);
    ThrowCompletionOr<Value> to_primitive(VM&, PreferredType preferred_type = PreferredType::Default) const;
    ThrowCompletionOr<GC::Ref<Object>> to_object(VM&) const;
    ThrowCompletionOr<Value> to_numeric(VM&) const;
    ThrowCompletionOr<Value> to_number(VM&) const;
    ThrowCompletionOr<GC::Ref<BigInt>> to_bigint(VM&) const;
    ThrowCompletionOr<i64> to_bigint_int64(VM&) const;
    ThrowCompletionOr<u64> to_bigint_uint64(VM&) const;
    ThrowCompletionOr<double> to_double(VM&) const;
    ThrowCompletionOr<PropertyKey> to_property_key(VM&) const;
    ThrowCompletionOr<i32> to_i32(VM&) const;
    ThrowCompletionOr<u32> to_u32(VM&) const;
    ThrowCompletionOr<i16> to_i16(VM&) const;
    ThrowCompletionOr<u16> to_u16(VM&) const;
    ThrowCompletionOr<i8> to_i8(VM&) const;
    ThrowCompletionOr<u8> to_u8(VM&) const;
    ThrowCompletionOr<u8> to_u8_clamp(VM&) const;
    ThrowCompletionOr<size_t> to_length(VM&) const;
    ThrowCompletionOr<size_t> to_index(VM&) const;
    ThrowCompletionOr<double> to_integer_or_infinity(VM&) const;
    bool to_boolean() const;

    ThrowCompletionOr<Value> get(VM&, PropertyKey const&) const;
    ThrowCompletionOr<GC::Ptr<FunctionObject>> get_method(VM&, PropertyKey const&) const;

    [[nodiscard]] String to_string_without_side_effects() const;

    Value value_or(Value fallback) const
    {
        if (is_empty())
            return fallback;
        return *this;
    }

    [[nodiscard]] GC::Ref<PrimitiveString> typeof_(VM&) const;

    bool operator==(Value const&) const;

    template<typename... Args>
    [[nodiscard]] ALWAYS_INLINE ThrowCompletionOr<Value> invoke(VM&, PropertyKey const& property_key, Args... args);

    // A double is any Value which does not have the full exponent and top mantissa bit set or has
    // exactly only those bits set.
    bool is_double() const { return (m_value.encoded & GC::CANON_NAN_BITS) != GC::CANON_NAN_BITS || (m_value.encoded == GC::CANON_NAN_BITS); }
    bool is_int32() const { return m_value.tag == INT32_TAG; }

    i32 as_i32() const
    {
        VERIFY(is_int32());
        return static_cast<i32>(m_value.encoded & 0xFFFFFFFF);
    }

    i32 as_i32_clamped_integral_number() const
    {
        VERIFY(is_int32() || is_finite_number());
        if (is_int32())
            return as_i32();
        double value = trunc(as_double());
        if (value > INT32_MAX)
            return INT32_MAX;
        if (value < INT32_MIN)
            return INT32_MIN;
        return static_cast<i32>(value);
    }

    bool to_boolean_slow_case() const;

private:
    ThrowCompletionOr<Value> to_number_slow_case(VM&) const;
    ThrowCompletionOr<Value> to_numeric_slow_case(VM&) const;
    ThrowCompletionOr<Value> to_primitive_slow_case(VM&, PreferredType) const;

    Value(u64 tag, u64 val)
    {
        ASSERT(!(tag & val));
        m_value.encoded = tag | val;
    }

    template<typename PointerType>
    Value(u64 tag, PointerType const* ptr)
    {
        if (!ptr) {
            // Make sure all nullptrs are null
            m_value.tag = NULL_TAG;
            return;
        }

        ASSERT((tag & 0x8000000000000000ul) == 0x8000000000000000ul);

        if constexpr (sizeof(PointerType*) < sizeof(u64)) {
            m_value.encoded = tag | reinterpret_cast<u32>(ptr);
        } else {
            // NOTE: Pointers in x86-64 use just 48 bits however are supposed to be
            //       sign extended up from the 47th bit.
            //       This means that all bits above the 47th should be the same as
            //       the 47th. When storing a pointer we thus drop the top 16 bits as
            //       we can recover it when extracting the pointer again.
            //       See also: NanBoxedValue::extract_pointer.
            m_value.encoded = tag | (reinterpret_cast<u64>(ptr) & 0x0000ffffffffffffULL);
        }
    }

    [[nodiscard]] ThrowCompletionOr<Value> invoke_internal(VM&, PropertyKey const&, Optional<GC::RootVector<Value>> arguments);

    ThrowCompletionOr<i32> to_i32_slow_case(VM&) const;

    friend Value js_undefined();
    friend Value js_null();
    friend ThrowCompletionOr<Value> greater_than(VM&, Value lhs, Value rhs);
    friend ThrowCompletionOr<Value> greater_than_equals(VM&, Value lhs, Value rhs);
    friend ThrowCompletionOr<Value> less_than(VM&, Value lhs, Value rhs);
    friend ThrowCompletionOr<Value> less_than_equals(VM&, Value lhs, Value rhs);
    friend ThrowCompletionOr<Value> add(VM&, Value lhs, Value rhs);
    friend bool same_value_non_number(Value lhs, Value rhs);
};

inline Value js_undefined()
{
    return Value(UNDEFINED_TAG << GC::TAG_SHIFT, (u64)0);
}

inline Value js_null()
{
    return Value(NULL_TAG << GC::TAG_SHIFT, (u64)0);
}

inline Value js_nan()
{
    return Value(NAN);
}

inline Value js_infinity()
{
    return Value(INFINITY);
}

inline Value js_negative_infinity()
{
    return Value(-INFINITY);
}

ThrowCompletionOr<Value> greater_than(VM&, Value lhs, Value rhs);
ThrowCompletionOr<Value> greater_than_equals(VM&, Value lhs, Value rhs);
ThrowCompletionOr<Value> less_than(VM&, Value lhs, Value rhs);
ThrowCompletionOr<Value> less_than_equals(VM&, Value lhs, Value rhs);
ThrowCompletionOr<Value> bitwise_and(VM&, Value lhs, Value rhs);
ThrowCompletionOr<Value> bitwise_or(VM&, Value lhs, Value rhs);
ThrowCompletionOr<Value> bitwise_xor(VM&, Value lhs, Value rhs);
ThrowCompletionOr<Value> bitwise_not(VM&, Value);
ThrowCompletionOr<Value> unary_plus(VM&, Value);
ThrowCompletionOr<Value> unary_minus(VM&, Value);
ThrowCompletionOr<Value> left_shift(VM&, Value lhs, Value rhs);
ThrowCompletionOr<Value> right_shift(VM&, Value lhs, Value rhs);
ThrowCompletionOr<Value> unsigned_right_shift(VM&, Value lhs, Value rhs);
ThrowCompletionOr<Value> add(VM&, Value lhs, Value rhs);
ThrowCompletionOr<Value> sub(VM&, Value lhs, Value rhs);
ThrowCompletionOr<Value> mul(VM&, Value lhs, Value rhs);
ThrowCompletionOr<Value> div(VM&, Value lhs, Value rhs);
ThrowCompletionOr<Value> mod(VM&, Value lhs, Value rhs);
ThrowCompletionOr<Value> exp(VM&, Value lhs, Value rhs);
ThrowCompletionOr<Value> in(VM&, Value lhs, Value rhs);
ThrowCompletionOr<Value> instance_of(VM&, Value lhs, Value rhs);
ThrowCompletionOr<Value> ordinary_has_instance(VM&, Value lhs, Value rhs);

ThrowCompletionOr<bool> is_loosely_equal(VM&, Value lhs, Value rhs);
bool is_strictly_equal(Value lhs, Value rhs);
bool same_value(Value lhs, Value rhs);
bool same_value_zero(Value lhs, Value rhs);
bool same_value_non_number(Value lhs, Value rhs);
ThrowCompletionOr<TriState> is_less_than(VM&, Value lhs, Value rhs, bool left_first);

double to_integer_or_infinity(double);

enum class NumberToStringMode {
    WithExponent,
    WithoutExponent,
};
[[nodiscard]] String number_to_string(double, NumberToStringMode = NumberToStringMode::WithExponent);
[[nodiscard]] ByteString number_to_byte_string(double, NumberToStringMode = NumberToStringMode::WithExponent);
double string_to_number(StringView);

inline bool Value::operator==(Value const& value) const { return same_value(*this, value); }

}

namespace AK {

static_assert(sizeof(JS::Value) == sizeof(double));

template<>
class Optional<JS::Value> : public OptionalBase<JS::Value> {
    template<typename U>
    friend class Optional;

public:
    using ValueType = JS::Value;

    Optional() = default;

    template<SameAs<OptionalNone> V>
    Optional(V) { }

    Optional(Optional<JS::Value> const& other)
    {
        if (other.has_value())
            m_value = other.m_value;
    }

    Optional(Optional&& other)
        : m_value(other.m_value)
    {
    }

    template<typename U = JS::Value>
    requires(!IsSame<OptionalNone, RemoveCVReference<U>>)
    explicit(!IsConvertible<U&&, JS::Value>) Optional(U&& value)
    requires(!IsSame<RemoveCVReference<U>, Optional<JS::Value>> && IsConstructible<JS::Value, U &&>)
        : m_value(forward<U>(value))
    {
    }

    template<SameAs<OptionalNone> V>
    Optional& operator=(V)
    {
        clear();
        return *this;
    }

    Optional& operator=(Optional const& other)
    {
        if (this != &other) {
            clear();
            m_value = other.m_value;
        }
        return *this;
    }

    Optional& operator=(Optional&& other)
    {
        if (this != &other) {
            clear();
            m_value = other.m_value;
        }
        return *this;
    }

    void clear()
    {
        m_value = {};
    }

    [[nodiscard]] bool has_value() const
    {
        return !m_value.is_empty();
    }

    [[nodiscard]] JS::Value& value() &
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] JS::Value const& value() const&
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] JS::Value value() &&
    {
        return release_value();
    }

    [[nodiscard]] JS::Value release_value()
    {
        VERIFY(has_value());
        JS::Value released_value = m_value;
        clear();
        return released_value;
    }

private:
    JS::Value m_value;
};

}

namespace GC {

template<>
class Root<JS::Value> {
public:
    Root() = default;

    static Root create(JS::Value value, SourceLocation location)
    {
        if (value.is_cell())
            return Root(value, &value.as_cell(), location);
        return Root(value);
    }

    auto cell() { return m_handle.cell(); }
    auto cell() const { return m_handle.cell(); }
    auto value() const { return *m_value; }
    bool is_null() const { return m_handle.is_null() && !m_value.has_value(); }

    bool operator==(JS::Value const& value) const { return value == m_value; }
    bool operator==(Root<JS::Value> const& other) const { return other.m_value == this->m_value; }

private:
    explicit Root(JS::Value value)
        : m_value(value)
    {
    }

    explicit Root(JS::Value value, Cell* cell, SourceLocation location)
        : m_value(value)
        , m_handle(Root<Cell>::create(cell, location))
    {
    }

    Optional<JS::Value> m_value;
    Root<Cell> m_handle;
};

inline Root<JS::Value> make_root(JS::Value value, SourceLocation location = SourceLocation::current())
{
    return Root<JS::Value>::create(value, location);
}

}

namespace AK {

template<>
struct Formatter<JS::Value> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, JS::Value value)
    {
        if (value.is_empty())
            return Formatter<StringView>::format(builder, "<empty>"sv);
        return Formatter<StringView>::format(builder, value.to_string_without_side_effects());
    }
};

template<>
struct Traits<JS::Value> : DefaultTraits<JS::Value> {
    static unsigned hash(JS::Value value) { return Traits<u64>::hash(value.encoded()); }
    static constexpr bool is_trivial() { return true; }
};

template<>
struct Traits<GC::Root<JS::Value>> : public DefaultTraits<GC::Root<JS::Value>> {
    static unsigned hash(GC::Root<JS::Value> const& handle) { return Traits<JS::Value>::hash(handle.value()); }
};

}
