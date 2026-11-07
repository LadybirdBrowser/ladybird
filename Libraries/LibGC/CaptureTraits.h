/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/DistinctNumeric.h>
#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/StdLibExtras.h>
#include <AK/Time.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Forward.h>
#include <LibGC/NanBoxedValue.h>
#include <LibGC/Ptr.h>

namespace GC {

template<typename T>
class Weak;

// A capture whose type the GC does not trace has to hold no cell edges at all. Non-GC objects keep the cells they use in
// GC::Root, so owning and counted pointers to them hold no edges of the function either. A raw pointer is never one of
// these, as it may point into a cell, such as at a mixin of one.
template<typename T>
struct IsUntracedCapture : FalseType { };

template<typename T>
requires(IsArithmetic<T> || IsEnum<T>)
struct IsUntracedCapture<T> : TrueType { };

template<>
struct IsUntracedCapture<Empty> : TrueType { };

template<typename R, typename... Args>
struct IsUntracedCapture<R (*)(Args...)> : TrueType { };

template<>
struct IsUntracedCapture<AK::String> : TrueType { };

template<>
struct IsUntracedCapture<AK::Utf16String> : TrueType { };

template<>
struct IsUntracedCapture<AK::FlyString> : TrueType { };

template<>
struct IsUntracedCapture<AK::Utf16FlyString> : TrueType { };

template<>
struct IsUntracedCapture<AK::ByteString> : TrueType { };

template<size_t inline_capacity>
struct IsUntracedCapture<AK::Detail::ByteBuffer<inline_capacity>> : TrueType { };

template<typename T>
struct IsUntracedCapture<Weak<T>> : TrueType { };

// NB: A root traces itself, so a value that holds one, such as a generated dictionary, holds nothing for its owner to
//     trace. GC::captures() of a root itself holds it as a pointer instead, so that a function never holds a root.
template<typename T>
struct IsUntracedCapture<Root<T>> : TrueType { };

template<typename T, size_t inline_capacity>
struct IsUntracedCapture<RootVector<T, inline_capacity>> : TrueType { };

template<typename T>
requires(!IsCellLike<RemoveCV<T>>)
struct IsUntracedCapture<AK::WeakPtr<T>> : TrueType { };

template<typename T, typename X, typename... Options>
struct IsUntracedCapture<AK::DistinctNumeric<T, X, Options...>> : IsUntracedCapture<T> { };

template<>
struct IsUntracedCapture<AK::Duration> : TrueType { };

template<>
struct IsUntracedCapture<AK::MonotonicTime> : TrueType { };

template<>
struct IsUntracedCapture<AK::UnixDateTime> : TrueType { };

template<>
struct IsUntracedCapture<AK::JsonValue> : TrueType { };

template<>
struct IsUntracedCapture<AK::JsonObject> : TrueType { };

template<>
struct IsUntracedCapture<AK::JsonArray> : TrueType { };

template<typename T>
requires(!IsCellLike<RemoveCV<T>>)
struct IsUntracedCapture<AK::RefPtr<T>> : TrueType { };

template<typename T>
requires(!IsCellLike<RemoveCV<T>>)
struct IsUntracedCapture<AK::NonnullRefPtr<T>> : TrueType { };

template<typename T>
requires(!IsCellLike<RemoveCV<T>>)
struct IsUntracedCapture<AK::OwnPtr<T>> : TrueType { };

template<typename T>
requires(!IsCellLike<RemoveCV<T>>)
struct IsUntracedCapture<AK::NonnullOwnPtr<T>> : TrueType { };

template<typename T>
struct IsUntracedCapture<Optional<T>> : IsUntracedCapture<T> { };

template<typename T, size_t inline_capacity>
struct IsUntracedCapture<Vector<T, inline_capacity>> : IsUntracedCapture<T> { };

template<typename T, size_t size>
struct IsUntracedCapture<Array<T, size>> : IsUntracedCapture<T> { };

template<typename... Ts>
struct IsUntracedCapture<Variant<Ts...>> {
    static constexpr bool value = (IsUntracedCapture<Ts>::value && ...);
};

template<typename T>
struct IsTracedCapture;

// Every part of a capture is either traced or holds no cell edges, so nothing a function captures can keep a cell
// that the GC does not see.
template<typename T>
inline constexpr bool IsCapturable = IsUntracedCapture<T>::value || IsTracedCapture<T>::value;

template<typename T>
struct IsTracedCapture {
    static constexpr bool value = IsBaseOf<NanBoxedValue, T> || (!IsCellLike<T> && requires(T& value, Cell::Visitor& visitor) { value.visit_edges(visitor); });
};

template<typename T>
struct IsTracedCapture<Ref<T>> : TrueType { };

template<typename T>
struct IsTracedCapture<Ptr<T>> : TrueType { };

template<typename T>
struct IsTracedCapture<Optional<T>> {
    static constexpr bool value = IsCapturable<T>;
};

template<typename T, size_t inline_capacity>
struct IsTracedCapture<Vector<T, inline_capacity>> {
    static constexpr bool value = IsCapturable<T>;
};

template<typename... Ts>
struct IsTracedCapture<Variant<Ts...>> {
    static constexpr bool value = (IsCapturable<Ts> && ...);
};

template<typename T>
requires(!IsCellLike<RemoveCV<T>>)
struct IsTracedCapture<AK::RefPtr<T>> : IsTracedCapture<T> { };

template<typename T>
requires(!IsCellLike<RemoveCV<T>>)
struct IsTracedCapture<AK::NonnullRefPtr<T>> : IsTracedCapture<T> { };

// Visits a member of a value type, such as a generated dictionary, from its visit_edges(). Every member must be traced or
// hold no cell edges, so that the value can be captured by a GC::Function.
template<typename Visitor, typename T>
void visit_capture_member(Visitor& visitor, T const& member)
{
    static_assert(IsCapturable<T>, "A member of a captured value must be traced, or hold no cell edges");
    if constexpr (IsVisitable<T>::value)
        visitor.visit(member);
}

}
