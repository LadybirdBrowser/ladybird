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

// A capture whose type the GC does not trace has to hold no cell edges at all. An owning or counted pointer is one if what
// it points to is, which a type declares with GC_UNTRACED_CAPTURE(). A raw pointer is never one of these, as it may point
// into a cell, such as at a mixin of one.
template<typename T>
struct IsUntracedCapture : FalseType { };

template<typename T>
requires(IsArithmetic<T> || IsEnum<T>)
struct IsUntracedCapture<T> : TrueType { };

template<typename T>
requires(requires { typename T::HoldsNoCellEdges; })
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
struct IsUntracedCapture<AK::RefPtr<T>> : IsUntracedCapture<RemoveCV<T>> { };

template<typename T>
requires(!IsCellLike<RemoveCV<T>>)
struct IsUntracedCapture<AK::NonnullRefPtr<T>> : IsUntracedCapture<RemoveCV<T>> { };

template<typename T>
requires(!IsCellLike<RemoveCV<T>>)
struct IsUntracedCapture<AK::OwnPtr<T>> : IsUntracedCapture<RemoveCV<T>> { };

template<typename T>
requires(!IsCellLike<RemoveCV<T>>)
struct IsUntracedCapture<AK::NonnullOwnPtr<T>> : IsUntracedCapture<RemoveCV<T>> { };

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
consteval bool is_capturable()
{
    // NB: A type that holds no cell edges may be incomplete here, which asking whether it is traced would not allow.
    if constexpr (IsUntracedCapture<T>::value)
        return true;
    else
        return IsTracedCapture<T>::value;
}

template<typename T>
inline constexpr bool IsCapturable = is_capturable<T>();

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

template<typename T, size_t size>
struct IsTracedCapture<Array<T, size>> {
    static constexpr bool value = IsCapturable<T>;
};

template<typename... Ts>
struct IsTracedCapture<Variant<Ts...>> {
    static constexpr bool value = (IsCapturable<Ts> && ...);
};

template<typename T>
requires(!IsCellLike<RemoveCV<T>>)
struct IsTracedCapture<AK::RefPtr<T>> : IsTracedCapture<RemoveCV<T>> { };

template<typename T>
requires(!IsCellLike<RemoveCV<T>>)
struct IsTracedCapture<AK::NonnullRefPtr<T>> : IsTracedCapture<RemoveCV<T>> { };

template<typename T>
inline constexpr bool IsRoot = false;

template<typename T>
inline constexpr bool IsRoot<Root<T>> = true;

template<typename T, size_t inline_capacity>
inline constexpr bool IsRoot<RootVector<T, inline_capacity>> = true;

// Visits a member of a value type, such as a generated dictionary, from its visit_edges(). Every member must be traced or
// hold no cell edges, so that the value can be captured by a GC::Function.
// NB: A root member would keep what it names alive for as long as the function lives, even through a cycle back to the
//     function, so a value with one isn't capturable.
template<typename Visitor, typename T>
void visit_capture_member(Visitor& visitor, T const& member)
{
    static_assert(!IsRoot<T>, "A captured value can't hold a root");
    static_assert(IsCapturable<T>, "A member of a captured value must be traced, or hold no cell edges");
    if constexpr (!IsUntracedCapture<T>::value) {
        static_assert(IsVisitable<T>::value, "A traced member of a captured value must be visitable");
        visitor.visit(member);
    }
}

}
