/*
 * Copyright (c) 2020-2021, Andreas Kling <kling@serenityos.org>
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
#include <AK/String.h>
#include <AK/Types.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/GCPtr.h>
#include <math.h>

namespace JS {

// 2 ** 53 - 1
static constexpr double MAX_ARRAY_LIKE_INDEX = 9007199254740991.0;
// Unique bit representation of negative zero (only sign bit set)
static constexpr u64 NEGATIVE_ZERO_BITS = ((u64)1 << 63);

static_assert(sizeof(double) == 8);
static_assert(sizeof(void*) == sizeof(double) || sizeof(void*) == sizeof(u32));
// To make our Value representation compact we can use the fact that IEEE
// doubles have a lot (2^52 - 2) of NaN bit patterns. The canonical form being
// just 0x7FF8000000000000 i.e. sign = 0 exponent is all ones and the top most
// bit of the mantissa set.
static constexpr u64 CANON_NAN_BITS = bit_cast<u64>(__builtin_nan(""));
static_assert(CANON_NAN_BITS == 0x7FF8000000000000);
// (Unfortunately all the other values are valid so we have to convert any
// incoming NaNs to this pattern although in practice it seems only the negative
// version of these CANON_NAN_BITS)
// +/- Infinity are represented by a full exponent but without any bits of the
// mantissa set.
static constexpr u64 POSITIVE_INFINITY_BITS = bit_cast<u64>(__builtin_huge_val());
static constexpr u64 NEGATIVE_INFINITY_BITS = bit_cast<u64>(-__builtin_huge_val());
static_assert(POSITIVE_INFINITY_BITS == 0x7FF0000000000000);
static_assert(NEGATIVE_INFINITY_BITS == 0xFFF0000000000000);
// However as long as any bit is set in the mantissa with the exponent of all
// ones this value is a NaN, and it even ignores the sign bit.
// (NOTE: we have to use __builtin_isnan here since some isnan implementations are not constexpr)
static_assert(__builtin_isnan(bit_cast<double>(0x7FF0000000000001)));
static_assert(__builtin_isnan(bit_cast<double>(0xFFF0000000040000)));
// This means we can use all of these NaNs to store all other options for Value.
// To make sure all of these other representations we use 0x7FF8 as the base top
// 2 bytes which ensures the value is always a NaN.
static constexpr u64 BASE_TAG = 0x7FF8;
// This leaves the sign bit and the three lower bits for tagging a value and then
// 48 bits of potential payload.
// First the pointer backed types (Object, String etc.), to signify this category
// and make stack scanning easier we use the sign bit (top most bit) of 1 to
// signify that it is a pointer backed type.
static constexpr u64 IS_CELL_BIT = 0x8000 | BASE_TAG;
// On all current 64-bit systems this code runs pointer actually only use the
// lowest 6 bytes which fits neatly into our NaN payload with the top two bytes
// left over for marking it as a NaN and tagging the type.
// Note that we do need to take care when extracting the pointer value but this
// is explained in the extract_pointer method.

// This leaves us 3 bits to tag the type of pointer:
static constexpr u64 OBJECT_TAG = 0b001 | IS_CELL_BIT;
static constexpr u64 STRING_TAG = 0b010 | IS_CELL_BIT;
static constexpr u64 SYMBOL_TAG = 0b011 | IS_CELL_BIT;
static constexpr u64 ACCESSOR_TAG = 0b100 | IS_CELL_BIT;
static constexpr u64 BIGINT_TAG = 0b101 | IS_CELL_BIT;

// We can then by extracting the top 13 bits quickly check if a Value is
// pointer backed.
static constexpr u64 IS_CELL_PATTERN = 0xFFF8ULL;
static_assert((OBJECT_TAG & IS_CELL_PATTERN) == IS_CELL_PATTERN);
static_assert((STRING_TAG & IS_CELL_PATTERN) == IS_CELL_PATTERN);
static_assert((CANON_NAN_BITS & IS_CELL_PATTERN) != IS_CELL_PATTERN);
static_assert((NEGATIVE_INFINITY_BITS & IS_CELL_PATTERN) != IS_CELL_PATTERN);

// Then for the non pointer backed types we don't set the sign bit and use the
// three lower bits for tagging as well.
static constexpr u64 UNDEFINED_TAG = 0b110 | BASE_TAG;
static constexpr u64 NULL_TAG = 0b111 | BASE_TAG;
static constexpr u64 BOOLEAN_TAG = 0b001 | BASE_TAG;
static constexpr u64 INT32_TAG = 0b010 | BASE_TAG;
static constexpr u64 EMPTY_TAG = 0b011 | BASE_TAG;
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

static constexpr u64 TAG_EXTRACTION = 0xFFFF000000000000;
static constexpr u64 TAG_SHIFT = 48;
static constexpr u64 SHIFTED_BOOLEAN_TAG = BOOLEAN_TAG << TAG_SHIFT;
static constexpr u64 SHIFTED_INT32_TAG = INT32_TAG << TAG_SHIFT;
static constexpr u64 SHIFTED_IS_CELL_PATTERN = IS_CELL_PATTERN << TAG_SHIFT;

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

class Value {
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
    bool is_cell() const { return (m_value.tag & IS_CELL_PATTERN) == IS_CELL_PATTERN; }
    ThrowCompletionOr<bool> is_array(VM&) const;
    bool is_function() const;
    bool is_constructor() const;
    ThrowCompletionOr<bool> is_regexp(VM&) const;

    bool is_nan() const
    {
        return m_value.encoded == CANON_NAN_BITS;
    }

    bool is_infinity() const
    {
        static_assert(NEGATIVE_INFINITY_BITS == (0x1ULL << 63 | POSITIVE_INFINITY_BITS));
        return (0x1ULL << 63 | m_value.encoded) == NEGATIVE_INFINITY_BITS;
    }

    bool is_positive_infinity() const
    {
        return m_value.encoded == POSITIVE_INFINITY_BITS;
    }

    bool is_negative_infinity() const
    {
        return m_value.encoded == NEGATIVE_INFINITY_BITS;
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
        : Value(EMPTY_TAG << TAG_SHIFT, (u64)0)
    {
    }

    template<typename T>
    requires(IsSameIgnoringCV<T, bool>) explicit Value(T value)
        : Value(BOOLEAN_TAG << TAG_SHIFT, (u64)value)
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
                m_value.encoded = CANON_NAN_BITS;
            else
                m_value.as_double = value;
        }
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

    Value(Object const* object)
        : Value(OBJECT_TAG << TAG_SHIFT, reinterpret_cast<void const*>(object))
    {
    }

    Value(PrimitiveString const* string)
        : Value(STRING_TAG << TAG_SHIFT, reinterpret_cast<void const*>(string))
    {
    }

    Value(Symbol const* symbol)
        : Value(SYMBOL_TAG << TAG_SHIFT, reinterpret_cast<void const*>(symbol))
    {
    }

    Value(Accessor const* accessor)
        : Value(ACCESSOR_TAG << TAG_SHIFT, reinterpret_cast<void const*>(accessor))
    {
    }

    Value(BigInt const* bigint)
        : Value(BIGINT_TAG << TAG_SHIFT, reinterpret_cast<void const*>(bigint))
    {
    }

    template<typename T>
    Value(GCPtr<T> ptr)
        : Value(ptr.ptr())
    {
    }

    template<typename T>
    Value(NonnullGCPtr<T> ptr)
        : Value(ptr.ptr())
    {
    }

    template<typename T>
    Value(Handle<T> const& ptr)
        : Value(ptr.ptr())
    {
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
    ThrowCompletionOr<NonnullGCPtr<PrimitiveString>> to_primitive_string(VM&);
    ThrowCompletionOr<Value> to_primitive(VM&, PreferredType preferred_type = PreferredType::Default) const;
    ThrowCompletionOr<NonnullGCPtr<Object>> to_object(VM&) const;
    ThrowCompletionOr<Value> to_numeric(VM&) const;
    ThrowCompletionOr<Value> to_number(VM&) const;
    ThrowCompletionOr<NonnullGCPtr<BigInt>> to_bigint(VM&) const;
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
    ThrowCompletionOr<GCPtr<FunctionObject>> get_method(VM&, PropertyKey const&) const;

    [[nodiscard]] String to_string_without_side_effects() const;

    Value value_or(Value fallback) const
    {
        if (is_empty())
            return fallback;
        return *this;
    }

    StringView typeof() const;

    bool operator==(Value const&) const;

    template<typename... Args>
    [[nodiscard]] ALWAYS_INLINE ThrowCompletionOr<Value> invoke(VM&, PropertyKey const& property_key, Args... args);

    static constexpr FlatPtr extract_pointer_bits(u64 encoded)
    {
#ifdef AK_ARCH_32_BIT
        // For 32-bit system the pointer fully fits so we can just return it directly.
        static_assert(sizeof(void*) == sizeof(u32));
        return static_cast<FlatPtr>(encoded & 0xffff'ffff);
#elif ARCH(X86_64) || ARCH(RISCV64)
        // For x86_64 and riscv64 the top 16 bits should be sign extending the "real" top bit (47th).
        // So first shift the top 16 bits away then using the right shift it sign extends the top 16 bits.
        return static_cast<FlatPtr>((static_cast<i64>(encoded << 16)) >> 16);
#elif ARCH(AARCH64) || ARCH(PPC64) || ARCH(PPC64LE)
        // For AArch64 the top 16 bits of the pointer should be zero.
        // For PPC64: all 64 bits can be used for pointers, however on Linux only
        //            the lower 43 bits are used for user-space addresses, so
        //            masking off the top 16 bits should match the rest of LibJS.
        return static_cast<FlatPtr>(encoded & 0xffff'ffff'ffffULL);
#else
#    error "Unknown architecture. Don't know whether pointers need to be sign-extended."
#endif
    }

    // A double is any Value which does not have the full exponent and top mantissa bit set or has
    // exactly only those bits set.
    bool is_double() const { return (m_value.encoded & CANON_NAN_BITS) != CANON_NAN_BITS || (m_value.encoded == CANON_NAN_BITS); }
    bool is_int32() const { return m_value.tag == INT32_TAG; }

    i32 as_i32() const
    {
        VERIFY(is_int32());
        return static_cast<i32>(m_value.encoded & 0xFFFFFFFF);
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
            //       See also: Value::extract_pointer.
            m_value.encoded = tag | (reinterpret_cast<u64>(ptr) & 0x0000ffffffffffffULL);
        }
    }

    template<typename PointerType>
    PointerType* extract_pointer() const
    {
        VERIFY(is_cell());
        return reinterpret_cast<PointerType*>(extract_pointer_bits(m_value.encoded));
    }

    [[nodiscard]] ThrowCompletionOr<Value> invoke_internal(VM&, PropertyKey const&, Optional<MarkedVector<Value>> arguments);

    ThrowCompletionOr<i32> to_i32_slow_case(VM&) const;

    union {
        double as_double;
        struct {
            u64 payload : 48;
            u64 tag : 16;
        };
        u64 encoded;
    } m_value { .encoded = 0 };

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
    return Value(UNDEFINED_TAG << TAG_SHIFT, (u64)0);
}

inline Value js_null()
{
    return Value(NULL_TAG << TAG_SHIFT, (u64)0);
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
class Optional<JS::Value> {
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

    template<typename O>
    ALWAYS_INLINE bool operator==(Optional<O> const& other) const
    {
        return has_value() == other.has_value() && (!has_value() || value() == other.value());
    }

    template<typename O>
    ALWAYS_INLINE bool operator==(O const& other) const
    {
        return has_value() && value() == other;
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

    JS::Value value_or(JS::Value const& fallback) const&
    {
        if (has_value())
            return value();
        return fallback;
    }

    [[nodiscard]] JS::Value value_or(JS::Value&& fallback) &&
    {
        if (has_value())
            return value();
        return fallback;
    }

    JS::Value const& operator*() const { return value(); }
    JS::Value& operator*() { return value(); }

    JS::Value const* operator->() const { return &value(); }
    JS::Value* operator->() { return &value(); }

private:
    JS::Value m_value;
};

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

}
