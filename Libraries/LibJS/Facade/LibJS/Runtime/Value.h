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
#include <AK/Format.h>
#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/SourceLocation.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibGC/NanBoxedValue.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibJS/Export.h>
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

// The Rust runtime encodes its values with exactly these bits, so a Value crosses the embedding ABI unchanged.
class JS_API Value : public GC::NanBoxedValue {
    template<typename T>
    static constexpr bool HasForbiddenDirectJSValueConversion = requires { typename RemoveCV<T>::JSValueConversionIsForbidden; };

public:
    [[nodiscard]] u16 tag() const { return m_value.tag; }

    bool is_special_empty_value() const { return m_value.encoded == (EMPTY_TAG << GC::TAG_SHIFT); }
    bool is_undefined() const { return m_value.encoded == (UNDEFINED_TAG << GC::TAG_SHIFT); }
    bool is_null() const { return m_value.encoded == (NULL_TAG << GC::TAG_SHIFT); }
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

    template<DerivedFrom<Object> T>
    [[nodiscard]] ALWAYS_INLINE bool is() const
    {
        return !!as_if<T>();
    }

    template<DerivedFrom<Object> T>
    [[nodiscard]] ALWAYS_INLINE GC::Ptr<T> as_if()
    {
        if (!is_object())
            return nullptr;
        if constexpr (IsSame<T, Object>) {
            return as_object();
        } else {
            return ::as_if<T>(as_object());
        }
    }

    template<DerivedFrom<Object> T>
    [[nodiscard]] ALWAYS_INLINE GC::Ptr<T const> as_if() const
    {
        if (!is_object())
            return nullptr;
        if constexpr (IsSame<T, Object>) {
            return as_object();
        } else {
            return ::as_if<T>(as_object());
        }
    }

    template<DerivedFrom<Object> T>
    [[nodiscard]] ALWAYS_INLINE T& as()
    {
        auto ptr = as_if<T>();
        VERIFY(ptr);
        return *ptr;
    }

    template<DerivedFrom<Object> T>
    [[nodiscard]] ALWAYS_INLINE T const& as() const
    {
        auto ptr = as_if<T>();
        VERIFY(ptr);
        return *ptr;
    }

    constexpr Value()
        : Value(UNDEFINED_TAG << GC::TAG_SHIFT, (u64)0)
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

    template<typename T>
    requires(HasForbiddenDirectJSValueConversion<T>) Value(T*) = delete;

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

    Value(BigInt const* bigint)
        : Value(BIGINT_TAG << GC::TAG_SHIFT, reinterpret_cast<void const*>(bigint))
    {
    }

    template<typename T>
    requires(!HasForbiddenDirectJSValueConversion<T>) Value(GC::Ptr<T> ptr)
        : Value(ptr.ptr())
    {
    }

    template<typename T>
    requires(HasForbiddenDirectJSValueConversion<T>) Value(GC::Ptr<T>) = delete;

    template<typename T>
    requires(!HasForbiddenDirectJSValueConversion<T>) Value(GC::Ref<T> ptr)
        : Value(ptr.ptr())
    {
    }

    template<typename T>
    requires(HasForbiddenDirectJSValueConversion<T>) Value(GC::Ref<T>) = delete;

    template<typename T>
    requires(!HasForbiddenDirectJSValueConversion<T>) Value(GC::Root<T> const& ptr)
        : Value(ptr.ptr())
    {
    }

    template<typename T>
    requires(HasForbiddenDirectJSValueConversion<T>) Value(GC::Root<T> const&) = delete;

    // Confirms the class of the cell this Value points at. The tag alone cannot do that: every
    // cell-backed tag names a different class, but a forged Value can carry an honest tag and
    // a pointer to a cell of some other class. The Rust runtime's cells keep their kind where
    // C++ cells do.
    ALWAYS_INLINE void verify_cell_kind(GC::CellKind kind) const
    {
        VERIFY(extract_pointer<GC::ForeignCell>()->cell_kind() == kind);
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
        ASSERT(is_number());
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
        verify_cell_kind(GC::CellKind::Object);
        return *extract_pointer<Object>();
    }

    Object const& as_object() const
    {
        VERIFY(is_object());
        verify_cell_kind(GC::CellKind::Object);
        return *extract_pointer<Object>();
    }

    PrimitiveString& as_string()
    {
        VERIFY(is_string());
        verify_cell_kind(GC::CellKind::PrimitiveString);
        return *extract_pointer<PrimitiveString>();
    }

    PrimitiveString const& as_string() const
    {
        VERIFY(is_string());
        verify_cell_kind(GC::CellKind::PrimitiveString);
        return *extract_pointer<PrimitiveString>();
    }

    Symbol& as_symbol()
    {
        VERIFY(is_symbol());
        verify_cell_kind(GC::CellKind::Symbol);
        return *extract_pointer<Symbol>();
    }

    Symbol const& as_symbol() const
    {
        VERIFY(is_symbol());
        verify_cell_kind(GC::CellKind::Symbol);
        return *extract_pointer<Symbol>();
    }

    BigInt const& as_bigint() const
    {
        VERIFY(is_bigint());
        verify_cell_kind(GC::CellKind::BigInt);
        return *extract_pointer<BigInt>();
    }

    BigInt& as_bigint()
    {
        VERIFY(is_bigint());
        verify_cell_kind(GC::CellKind::BigInt);
        return *extract_pointer<BigInt>();
    }

    FunctionObject& as_function();
    FunctionObject const& as_function() const;

    u64 encoded() const { return m_value.encoded; }

    ThrowCompletionOr<Utf16String> to_utf16_string(VM&) const;
    ThrowCompletionOr<GC::Ref<Object>> to_object(VM&) const;
    ThrowCompletionOr<GC::Ref<Object>> to_object_slow(VM&) const;
    ThrowCompletionOr<Value> to_number(VM&) const;
    ThrowCompletionOr<GC::Ref<BigInt>> to_bigint(VM&) const;
    ThrowCompletionOr<u64> to_bigint_uint64(VM&) const;
    ThrowCompletionOr<double> to_double(VM&) const;
    ThrowCompletionOr<i32> to_i32(VM&) const;
    ThrowCompletionOr<u32> to_u32(VM&) const;
    ThrowCompletionOr<u16> to_u16(VM&) const;
    ThrowCompletionOr<u8> to_u8(VM&) const;
    ThrowCompletionOr<size_t> to_length(VM&) const;
    bool to_boolean() const;

    ThrowCompletionOr<Value> get(VM&, PropertyKey const&) const;

    ThrowCompletionOr<GC::Ptr<FunctionObject>> get_method(VM&, PropertyKey const&) const;

    [[nodiscard]] Utf16String to_utf16_string_without_side_effects() const;

    bool operator==(Value const&) const;

    // A double is any Value which does not have the full exponent and top mantissa bit set or has
    // exactly only those bits set.
    bool is_double() const { return (m_value.encoded & GC::CANON_NAN_BITS) != GC::CANON_NAN_BITS || (m_value.encoded == GC::CANON_NAN_BITS); }
    bool is_int32() const { return m_value.tag == INT32_TAG; }

    i32 as_i32() const
    {
        ASSERT(is_int32());
        return static_cast<i32>(m_value.encoded & 0xFFFFFFFF);
    }

    bool to_boolean_slow_case() const;

private:
    ThrowCompletionOr<Value> to_number_slow_case(VM&) const;

    enum class EmptyTag { Empty };

    constexpr Value(EmptyTag)
        : Value(EMPTY_TAG << GC::TAG_SHIFT, (u64)0)
    {
    }

    constexpr Value(u64 tag, u64 val)
    {
        ASSERT(!(tag & val));
        m_value.encoded = tag | val;
    }

    template<typename PointerType>
    Value(u64 tag, PointerType const* ptr)
    {
        if (!ptr) {
            m_value.tag = NULL_TAG;
            return;
        }

        ASSERT((tag & 0x8000000000000000ul) == 0x8000000000000000ul);
        m_value.encoded = tag | GC::NanBoxedValue::encode_pointer_bits(ptr);
    }

    friend constexpr Value js_undefined();
    friend constexpr Value js_null();
    friend constexpr Value js_special_empty_value();
};

inline constexpr Value js_undefined()
{
    return Value(UNDEFINED_TAG << GC::TAG_SHIFT, (u64)0);
}

inline constexpr Value js_null()
{
    return Value(NULL_TAG << GC::TAG_SHIFT, (u64)0);
}

inline constexpr Value js_special_empty_value()
{
    return Value(Value::EmptyTag::Empty);
}

JS_API bool same_value(Value lhs, Value rhs);
JS_API bool same_value_zero(Value lhs, Value rhs);

enum class NumberToStringMode {
    WithExponent,
    WithoutExponent,
};
JS_API void number_to_string(StringBuilder&, double, NumberToStringMode = NumberToStringMode::WithExponent);
[[nodiscard]] JS_API Utf16String number_to_utf16_string(double, NumberToStringMode = NumberToStringMode::WithExponent);

inline bool Value::operator==(Value const& value) const { return same_value(*this, value); }

}

namespace AK {

static_assert(sizeof(JS::Value) == sizeof(double));

template<>
struct SentinelOptionalTraits<JS::Value> {
    static constexpr JS::Value sentinel_value() { return JS::js_special_empty_value(); }
    static constexpr bool is_sentinel(JS::Value const& value) { return value.is_special_empty_value(); }
};

template<>
class Optional<JS::Value> : public SentinelOptional<JS::Value> {
public:
    using SentinelOptional::SentinelOptional;
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
struct Formatter<JS::Value> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, JS::Value value)
    {
        if (value.is_special_empty_value())
            return Formatter<StringView>::format(builder, "<empty>"sv);
        return Formatter<Utf16String> {}.format(builder, value.to_utf16_string_without_side_effects());
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
