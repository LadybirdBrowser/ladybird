/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DefaultDelete.h>
#include <AK/SinglyLinkedListSizePolicy.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>

namespace AK {

namespace Detail {

template<size_t inline_capacity>
class ByteBuffer;

class StringData;

}

enum class TrailingCodePointTransformation : u8;

class BigEndianInputBitStream;
class BigEndianOutputBitStream;
class Bitmap;
class ByteString;
class ByteStringImpl;
class CircularBuffer;
class ConstrainedStream;
class CountingStream;
class Duration;
class Error;
class FlyString;
class GenericLexer;
class IPv4Address;
class IPv6Address;
class JsonArray;
class JsonObject;
class JsonValue;
class LexicalPath;
class LittleEndianInputBitStream;
class LittleEndianOutputBitStream;
class SearchableCircularBuffer;
class SeekableStream;
class StackInfo;
class Stream;
class String;
class StringBuilder;
class StringView;
class UnixDateTime;
class Utf16View;
class Utf32CodePointIterator;
class Utf32View;
class Utf8CodePointIterator;
class Utf8View;

using ByteBuffer = Detail::ByteBuffer<32>;

template<typename T>
class Span;

template<typename T, size_t Size>
struct Array;

template<typename Container, typename ValueType>
class SimpleIterator;

template<typename T>
using ReadonlySpan = Span<T const>;

using ReadonlyBytes = ReadonlySpan<u8>;
using Bytes = Span<u8>;

template<typename T, AK::MemoryOrder DefaultMemoryOrder>
class Atomic;

template<typename T, typename TSizeCalculationPolicy = DefaultSizeCalculationPolicy>
class SinglyLinkedList;

template<typename T>
class DoublyLinkedList;

template<typename T, size_t capacity>
class CircularQueue;

template<typename T>
struct Traits;

template<typename T, typename TraitsForT = Traits<T>, bool IsOrdered = false>
class HashTable;

template<typename T, typename TraitsForT = Traits<T>>
using OrderedHashTable = HashTable<T, TraitsForT, true>;

template<typename K, typename V, typename KeyTraits = Traits<K>, typename ValueTraits = Traits<V>, bool IsOrdered = false>
class HashMap;

template<typename K, typename V, typename KeyTraits = Traits<K>, typename ValueTraits = Traits<V>>
using OrderedHashMap = HashMap<K, V, KeyTraits, ValueTraits, true>;

template<typename T>
class Badge;

template<typename T>
class FixedArray;

template<size_t precision, typename Underlying = i32>
class FixedPoint;

template<typename>
class Function;

template<typename Out, typename... In>
class Function<Out(In...)>;

template<typename T>
class NonnullRefPtr;

template<typename T>
class NonnullOwnPtr;

template<typename T>
class Optional;

template<>
class Optional<String>;

template<>
class Optional<FlyString>;

template<typename T>
class RefPtr;

template<typename T, typename TDeleter = DefaultDelete<T>>
class OwnPtr;

template<typename T>
class WeakPtr;

template<typename T, size_t inline_capacity = 0>
requires(!IsRvalueReference<T>) class Vector;

template<typename T, typename ErrorType = Error>
class [[nodiscard]] ErrorOr;

}

#if USING_AK_GLOBALLY
using AK::Array;
using AK::Atomic;
using AK::Badge;
using AK::BigEndianInputBitStream;
using AK::BigEndianOutputBitStream;
using AK::Bitmap;
using AK::ByteBuffer;
using AK::Bytes;
using AK::ByteString;
using AK::CircularBuffer;
using AK::CircularQueue;
using AK::ConstrainedStream;
using AK::CountingStream;
using AK::DoublyLinkedList;
using AK::Error;
using AK::ErrorOr;
using AK::FixedArray;
using AK::FlyString;
using AK::Function;
using AK::GenericLexer;
using AK::HashMap;
using AK::HashTable;
using AK::IPv4Address;
using AK::IPv6Address;
using AK::JsonArray;
using AK::JsonObject;
using AK::JsonValue;
using AK::LexicalPath;
using AK::LittleEndianInputBitStream;
using AK::LittleEndianOutputBitStream;
using AK::NonnullOwnPtr;
using AK::NonnullRefPtr;
using AK::Optional;
using AK::OwnPtr;
using AK::ReadonlyBytes;
using AK::RefPtr;
using AK::SearchableCircularBuffer;
using AK::SeekableStream;
using AK::SinglyLinkedList;
using AK::Span;
using AK::StackInfo;
using AK::Stream;
using AK::String;
using AK::StringBuilder;
using AK::StringView;
using AK::TrailingCodePointTransformation;
using AK::Traits;
using AK::UnixDateTime;
using AK::Utf16View;
using AK::Utf32CodePointIterator;
using AK::Utf32View;
using AK::Utf8CodePointIterator;
using AK::Utf8View;
using AK::Vector;

#endif
