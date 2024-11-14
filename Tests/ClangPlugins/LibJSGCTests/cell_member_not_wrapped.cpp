/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -cc1 -verify %plugin_opts% %s 2>&1

#include <LibJS/Runtime/Object.h>

// Ensure it can see through typedefs
typedef JS::Object NewType1;
using NewType2 = JS::Object;

class TestClass : public JS::Object {
    JS_OBJECT(TestClass, JS::Object);

public:
    explicit TestClass(JS::Realm& realm, JS::Object& obj)
        : JS::Object(realm, nullptr)
        , m_object_ref(obj)
    {
    }

private:
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_object_ref);
        visitor.visit(m_object_ptr);
    }

    // expected-error@+1 {{reference to GC::Cell type should be wrapped in GC::Ref}}
    JS::Object& m_object_ref;
    // expected-error@+1 {{pointer to GC::Cell type should be wrapped in GC::Ptr}}
    JS::Object* m_object_ptr;
    // expected-error@+1 {{pointer to GC::Cell type should be wrapped in GC::Ptr}}
    Vector<JS::Object*> m_objects;
    // expected-error@+1 {{pointer to GC::Cell type should be wrapped in GC::Ptr}}
    NewType1* m_newtype_1;
    // expected-error@+1 {{pointer to GC::Cell type should be wrapped in GC::Ptr}}
    NewType2* m_newtype_2;
};

class TestClassNonCell {
public:
    explicit TestClassNonCell(JS::Object& obj)
        : m_object_ref(obj)
    {
    }

private:
    // expected-error@+1 {{reference to GC::Cell type should be wrapped in GC::Ref}}
    JS::Object& m_object_ref;
    // expected-error@+1 {{pointer to GC::Cell type should be wrapped in GC::Ptr}}
    JS::Object* m_object_ptr;
    // expected-error@+1 {{pointer to GC::Cell type should be wrapped in GC::Ptr}}
    Vector<JS::Object*> m_objects;
    // expected-error@+1 {{pointer to GC::Cell type should be wrapped in GC::Ptr}}
    NewType1* m_newtype_1;
    // expected-error@+1 {{pointer to GC::Cell type should be wrapped in GC::Ptr}}
    NewType2* m_newtype_2;
};
