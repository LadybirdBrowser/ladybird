/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibJS/Forward.h>

namespace JS {

// The base of the types through which C++ names the cells of the Rust runtime: objects, strings, symbols, BigInts,
// realms and the rest. A pointer to such a type is the runtime's cell itself, so these types add neither fields nor
// virtual functions, and only the runtime creates and destroys their cells. Like the embedder's own cells, they offer
// vm() as well.
class EngineCell : public GC::ForeignCell {
public:
    ALWAYS_INLINE VM& vm() const;

    template<typename T>
    bool fast_is() const = delete;
};

}
