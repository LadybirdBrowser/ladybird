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
    JS_DECLARE_ALLOCATOR(NumberObject);

public:
    static NonnullGCPtr<NumberObject> create(Realm&, double);

    virtual ~NumberObject() override = default;

    double number() const { return m_value; }

protected:
    NumberObject(double, Object& prototype);

private:
    double m_value { 0 };
};

}
