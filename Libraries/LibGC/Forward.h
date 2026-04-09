/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibGC/Export.h>

namespace GC {

class Cell;
class CellAllocator;
class DeferGC;
class RootImpl;
class Heap;
class HeapBlock;
class NanBoxedValue;
class Timer;
class WeakContainer;
class WeakImpl;

template<typename T>
class Function;

template<typename T>
class HeapHashTable;

template<class T>
class Root;

template<class T>
class Ref;

template<class T>
class Ptr;

template<class T, size_t inline_capacity = 0>
class ConservativeVector;

template<typename K, typename V, typename KeyTraits, typename ValueTraits, bool IsOrdered>
class ConservativeHashMap;

template<typename T, typename TraitsForT, bool IsOrdered>
class ConservativeHashTable;

template<class T>
class HeapVector;

template<class T, size_t inline_capacity = 0>
class RootVector;

template<typename T, typename TraitsForT, bool IsOrdered>
class RootHashTable;

template<typename K, typename V>
class WeakHashMap;

}
