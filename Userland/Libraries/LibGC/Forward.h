/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace GC {
class Cell;
class CellAllocator;
class DeferGC;
class HandleImpl;
class Heap;
class HeapBlock;
class NanBoxedValue;
class WeakContainer;

template<class T, size_t inline_capacity = 0>
class ConservativeVector;

template<class T>
class Handle;

template<typename T>
class Function;

template<class T, size_t inline_capacity = 0>
class MarkedVector;
}
