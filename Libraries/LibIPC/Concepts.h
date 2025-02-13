/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibCore/SharedCircularQueue.h>

#pragma once

// These concepts are used to help the compiler distinguish between specializations that would be
// ambiguous otherwise. For example, if the specializations for int and Vector<T> were declared as
// follows:
//
//     template<> ErrorOr<int> decode(Decoder& decoder);
//     template<typename T> ErrorOr<Vector<T>> decode(Decoder& decoder);
//
// Then decode<int>() would be ambiguous because either declaration could work (the compiler would
// not be able to distinguish if you wanted to decode an int or a Vector of int).
//
// They also serve to work around the inability to do partial function specialization in C++.
namespace IPC::Concepts {

namespace Detail {

// Cannot use SpecializationOf with these templates because they have non-type parameters. See https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p1985r3.pdf
template<typename T>
constexpr inline bool IsArray = false;
template<typename T, size_t N>
constexpr inline bool IsArray<Array<T, N>> = true;

template<typename T>
constexpr inline bool IsVector = false;
template<typename T, size_t inline_capacity>
constexpr inline bool IsVector<Vector<T, inline_capacity>> = true;

template<typename T>
constexpr inline bool IsHashMap = false;
template<typename K, typename V, typename KeyTraits, typename ValueTraits, bool IsOrdered>
constexpr inline bool IsHashMap<HashMap<K, V, KeyTraits, ValueTraits, IsOrdered>> = true;

template<typename T>
constexpr inline bool IsSharedSingleProducerCircularQueue = false;
template<typename T, size_t Size>
constexpr inline bool IsSharedSingleProducerCircularQueue<Core::SharedSingleProducerCircularQueue<T, Size>> = true;

}

template<typename T>
concept Array = Detail::IsArray<T>;

template<typename T>
concept Vector = Detail::IsVector<T>;

template<typename T>
concept HashMap = Detail::IsHashMap<T>;

template<typename T>
concept SharedSingleProducerCircularQueue = Detail::IsSharedSingleProducerCircularQueue<T>;

template<typename T>
concept Optional = SpecializationOf<T, AK::Optional>;

template<typename T>
concept Variant = SpecializationOf<T, AK::Variant>;

}
