/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>

namespace JS {

class NumberObject : public Object {
    JS_OBJECT(NumberObject, Object);
    GC_DECLARE_ALLOCATOR(NumberObject);

public:
    static GC::Ref<NumberObject> create(Realm&, double);

    virtual ~NumberObject() override = default;

    double number() const { return m_value; }

protected:
    NumberObject(double, Object& prototype);

private:
    virtual bool is_number_object() const final { return true; }

    double m_value { 0 };
};

template<>
inline bool Object::fast_is<NumberObject>() const { return is_number_object(); }

}
