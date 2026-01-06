/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <AK/Platform.h>

// Test the must_upcall attribute enforcement

class Base {
public:
    MUST_UPCALL virtual void must_call_base() { }

    virtual void optional_call_base() { }
};

class DerivedGood : public Base {
public:
    void must_call_base() override
    {
        Base::must_call_base(); // OK - calls base
    }

    void optional_call_base() override
    {
        // OK - no must_upcall on this method
    }
};

class DerivedBad : public Base {
public:
    // expected-error@+1 {{Missing call to Base::must_call_base (required by must_upcall attribute)}}
    void must_call_base() override
    {
        // Missing call to Base::must_call_base!
    }
};

// Test that the attribute propagates through inheritance
class DerivedFromDerived : public DerivedGood {
public:
    // expected-error@+1 {{Missing call to Base::must_call_base (required by must_upcall attribute)}}
    void must_call_base() override
    {
        // Should still require upcall even though DerivedGood doesn't have the annotation
    }
};

class DerivedFromDerivedGood : public DerivedGood {
public:
    void must_call_base() override
    {
        DerivedGood::must_call_base(); // OK - calls immediate parent
    }
};

// Using Base:: should also work (common pattern with Base typedef)
class DerivedFromDerivedAlsoGood : public DerivedGood {
public:
    void must_call_base() override
    {
        Base::must_call_base(); // Also OK - Base:: is accepted
    }
};

// Test with namespaced classes (qualified name support)
namespace NS {

class NamespacedBase {
public:
    MUST_UPCALL virtual void foo() { }
};

class NamespacedDerived : public NamespacedBase {
public:
    void foo() override
    {
        NamespacedBase::foo(); // OK - unqualified parent name
    }
};

} // namespace NS

class DerivedFromNamespaced : public NS::NamespacedDerived {
public:
    void foo() override
    {
        NS::NamespacedDerived::foo(); // OK - qualified parent name
    }
};
