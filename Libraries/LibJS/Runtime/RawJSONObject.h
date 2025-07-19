/*
 * Copyright (c) 2025, Artsiom Yafremau <aplefull@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class RawJSONObject final : public Object {
    JS_OBJECT(RawJSONObject, Object);
    GC_DECLARE_ALLOCATOR(RawJSONObject);

public:
    static GC::Ref<RawJSONObject> create(Realm& realm, Object* prototype);
    virtual ~RawJSONObject() override = default;

private:
    explicit RawJSONObject(Shape& shape)
        : Object(shape)
    {
    }

    RawJSONObject(ConstructWithPrototypeTag tag, Object& prototype)
        : Object(tag, prototype)
    {
    }

    virtual bool is_raw_json_object() const final { return true; }
};

template<>
inline bool Object::fast_is<RawJSONObject>() const { return is_raw_json_object(); }

}
