/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// Declares inside a type that it holds no cell edges, for a type that can't be named at global scope.
#define GC_HOLDS_NO_CELL_EDGES using HoldsNoCellEdges = void
