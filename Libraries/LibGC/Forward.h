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
class WeakContainer;
class WeakImpl;

template<typename T>
class Function;

template<typename T>
class HeapHashTable;

template<class T>
class Root;

template<class T, size_t inline_capacity = 0>
class ConservativeVector;

template<class T>
class HeapVector;

template<class T, size_t inline_capacity = 0>
class RootVector;

}
