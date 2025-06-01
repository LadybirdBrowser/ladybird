/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/MapIterator.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS {

class MapIteratorPrototype final : public PrototypeObject<MapIteratorPrototype, MapIterator> {
    JS_PROTOTYPE_OBJECT(MapIteratorPrototype, MapIterator, MapIterator);
    GC_DECLARE_ALLOCATOR(MapIteratorPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~MapIteratorPrototype() override = default;

    virtual bool is_map_iterator_prototype() const override { return true; }

private:
    MapIteratorPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(next);
};

template<>
inline bool Object::fast_is<MapIteratorPrototype>() const { return is_map_iterator_prototype(); }

}
