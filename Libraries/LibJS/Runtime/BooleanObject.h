/*
 * Copyright (c) 2020, Jack Karamanian <karamanian.jack@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>

namespace JS {

class BooleanObject : public Object {
    JS_OBJECT(BooleanObject, Object);
    GC_DECLARE_ALLOCATOR(BooleanObject);

public:
    static GC::Ref<BooleanObject> create(Realm&, bool);

    virtual ~BooleanObject() override = default;

    bool boolean() const { return m_value; }

protected:
    BooleanObject(bool, Object& prototype);

private:
    virtual bool is_boolean_object() const final { return true; }

    bool m_value { false };
};

template<>
inline bool Object::fast_is<BooleanObject>() const { return is_boolean_object(); }

}
