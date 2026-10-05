/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BitCast.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibJS/Embedding/ABI.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/Symbol.h>
#include <LibJS/Runtime/Value.h>

// Conversions between the facade's types and those of the Rust runtime's embedding ABI. Only the facade's own .cpp
// files may include this header, because it includes the runtime's C ABI.
//
// A facade handle is the runtime's cell itself, and the facade's JS::VM holds the runtime's VM in its first bytes, so
// both cross as reinterpreted pointers. Values and property keys have the same bits in both.

namespace JS::EmbeddingABI {

inline JSVM* vm_to_abi(VM& vm)
{
    return reinterpret_cast<JSVM*>(&vm);
}

inline JSValue value_to_abi(Value value)
{
    return value.encoded();
}

inline Value value_from_abi(JSValue value)
{
    return bit_cast<Value>(value);
}

// The key is borrowed: the runtime takes no reference to its string.
inline JSPropertyKey const* property_key_to_abi(PropertyKey const& property_key)
{
    static_assert(sizeof(PropertyKey) == sizeof(JSPropertyKey));
    static_assert(alignof(PropertyKey) == alignof(JSPropertyKey));
    return reinterpret_cast<JSPropertyKey const*>(&property_key);
}

template<typename AbiCell, typename FacadeCell>
AbiCell* cell_to_abi(FacadeCell const& cell)
{
    static_assert(IsBaseOf<GC::ForeignCell, FacadeCell>);
    return reinterpret_cast<AbiCell*>(const_cast<FacadeCell*>(&cell));
}

template<typename FacadeCell, typename AbiCell>
FacadeCell* cell_from_abi(AbiCell* cell)
{
    static_assert(IsBaseOf<GC::ForeignCell, FacadeCell>);
    return reinterpret_cast<FacadeCell*>(cell);
}

inline JSObject* object_to_abi(Object const& object)
{
    return cell_to_abi<JSObject>(object);
}

inline Object& object_from_abi(JSObject* object)
{
    VERIFY(object);
    return *cell_from_abi<Object>(object);
}

// For the facade types that only a later part of the facade defines, which C++ can name before it knows them.
template<typename FacadeCell>
FacadeCell& declared_cell_from_abi(void* cell)
{
    VERIFY(cell);
    return *static_cast<FacadeCell*>(cell);
}

template<typename AbiCell, typename FacadeCell>
AbiCell* declared_cell_to_abi(FacadeCell& cell)
{
    return static_cast<AbiCell*>(static_cast<void*>(&cell));
}

// A key that the runtime lends for the duration of a call, which is not given a reference to its string.
inline PropertyKey const& lent_property_key_from_abi(JSPropertyKey const& property_key)
{
    return *reinterpret_cast<PropertyKey const*>(&property_key);
}

inline JSPrimitiveString* primitive_string_to_abi(PrimitiveString const& string)
{
    return cell_to_abi<JSPrimitiveString>(string);
}

inline JSSymbol* symbol_to_abi(Symbol const& symbol)
{
    return cell_to_abi<JSSymbol>(symbol);
}

inline JSBigInt* bigint_to_abi(BigInt const& bigint)
{
    return cell_to_abi<JSBigInt>(bigint);
}

// The code units stay where they are, so the view is only valid while its string is.
inline JSUtf16View utf16_view_to_abi(Utf16View const& view)
{
    if (view.has_ascii_storage())
        return { view.ascii_span().data(), view.length_in_code_units(), true };
    return { view.utf16_span().data(), view.length_in_code_units(), false };
}

inline Utf16View utf16_view_from_abi(JSUtf16View view)
{
    if (view.length_in_code_units == 0)
        return {};
    if (view.has_ascii_storage)
        return Utf16View { StringView { static_cast<char const*>(view.data), view.length_in_code_units } };
    return Utf16View { static_cast<char16_t const*>(view.data), view.length_in_code_units };
}

// The receiver adopts the reference that the string's raw word carries.
inline JSOwnedUtf16String owned_utf16_string_to_abi(Utf16String string)
{
    return move(string).into_raw();
}

inline Utf16String owned_utf16_string_from_abi(JSOwnedUtf16String string)
{
    return Utf16String::adopt_raw(string);
}

namespace Detail {

template<typename T>
struct CellPointerFromPayload;

template<typename T>
struct CellPointerFromPayload<GC::Ref<T>> {
    static GC::Ref<T> from_payload(u64 payload)
    {
        auto* cell = reinterpret_cast<T*>(static_cast<FlatPtr>(payload));
        VERIFY(cell);
        return *cell;
    }
};

template<typename T>
struct CellPointerFromPayload<GC::Ptr<T>> {
    static GC::Ptr<T> from_payload(u64 payload)
    {
        return reinterpret_cast<T*>(static_cast<FlatPtr>(payload));
    }
};

}

// A completion of the runtime, whose normal payload is a JSValue, a bool, or a cell pointer (null for none); an
// operation that writes its result to an out parameter leaves the payload 0. A throw that comes out of the runtime was
// thrown there, so this does not pass it through throw_completion(), which is where C++ code starts a throw.
template<typename T>
ThrowCompletionOr<T> completion_from_abi(JSCompletion completion)
{
    if (completion.variant == JS_COMPLETION_THROW)
        return Completion { Completion::Type::Throw, value_from_abi(completion.payload) };
    VERIFY(completion.variant == JS_COMPLETION_NORMAL);
    if constexpr (IsSame<T, void>) {
        return {};
    } else if constexpr (IsSame<T, bool>) {
        return completion.payload != 0;
    } else if constexpr (IsSame<T, Value>) {
        return value_from_abi(completion.payload);
    } else if constexpr (IsEnum<T>) {
        return static_cast<T>(completion.payload);
    } else {
        return Detail::CellPointerFromPayload<T>::from_payload(completion.payload);
    }
}

// A completion for the runtime, which the embedder's hooks return to it: a throw completion carries the thrown value,
// and a normal one a JSValue, an ABI enumerator, or 0 for none.
template<typename T>
JSCompletion completion_to_abi(ThrowCompletionOr<T> const& completion)
{
    if (completion.is_throw_completion())
        return { value_to_abi(completion.throw_completion().value()), JS_COMPLETION_THROW };
    if constexpr (IsSame<T, void>)
        return { 0, JS_COMPLETION_NORMAL };
    else if constexpr (IsSame<T, Value>)
        return { value_to_abi(completion.value()), JS_COMPLETION_NORMAL };
    else if constexpr (IsEnum<T>)
        return { static_cast<u64>(to_underlying(completion.value())), JS_COMPLETION_NORMAL };
    else
        static_assert(DependentFalse<T>, "A hook returns a value, an ABI enumerator or nothing");
}

// The value that a completion of the runtime threw, which must be a throw completion. The runtime threw it, and logged
// it then if it logs exceptions, so this does not pass it through throw_completion() either.
inline Completion throw_completion_from_abi(JSCompletion completion)
{
    VERIFY(completion.variant == JS_COMPLETION_THROW);
    return Completion { Completion::Type::Throw, value_from_abi(completion.payload) };
}

}
