/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibGC/Weak.h>

struct Params {
    virtual ~Params() = default;
};

struct ParamsWithCell : Params {
    // expected-error@+1 {{Member cell of polymorphic class ParamsWithCell contains GC pointers but ParamsWithCell has no visit_edges method}}
    GC::Ref<GC::Cell> cell;
};

struct ParamsWithCells : Params {
    // expected-error@+1 {{Member cells of polymorphic class ParamsWithCells contains GC pointers but ParamsWithCells has no visit_edges method}}
    Vector<GC::Ptr<GC::Cell>> cells;
};

struct DerivedParamsWithCell : ParamsWithCells {
    // expected-error@+1 {{Member other_cell of polymorphic class DerivedParamsWithCell contains GC pointers but DerivedParamsWithCell has no visit_edges method}}
    GC::Ptr<GC::Cell> other_cell;
};

template<typename T>
struct TemplateParams : Params {
    // expected-error@+1 {{Member cell of polymorphic class TemplateParams<GC::Ref<GC::Cell>> contains GC pointers but TemplateParams<GC::Ref<GC::Cell>> has no visit_edges method}}
    T cell;
};

static_assert(sizeof(TemplateParams<GC::Ref<GC::Cell>>) > 0);
static_assert(sizeof(TemplateParams<int>) > 0);

struct VisitedParams {
    virtual ~VisitedParams() = default;
    virtual void visit_edges(GC::Cell::Visitor&) { }
};

struct VisitedParamsWithUnvisitedCell : VisitedParams {
    // expected-error@+1 {{Member cell of polymorphic class VisitedParamsWithUnvisitedCell contains GC pointers but VisitedParamsWithUnvisitedCell has no visit_edges method}}
    GC::Ref<GC::Cell> cell;
};

struct VisitedParamsWithCell : VisitedParams {
    virtual void visit_edges(GC::Cell::Visitor& visitor) override
    {
        VisitedParams::visit_edges(visitor);
        visitor.visit(cell);
    }

    GC::Ref<GC::Cell> cell;
};

struct ParamsWithRootedOrUntracedCells : Params {
    GC::Root<GC::Cell> rooted_cell;
    GC::RawPtr<GC::Cell> untraced_cell;
    GC::Weak<GC::Cell> weak_cell;
};

struct PolymorphicHolder {
    virtual ~PolymorphicHolder() = default;
    GC::Ref<GC::Cell> cell;
};

struct Holder {
    GC::Ref<GC::Cell> cell;
};

struct DerivedHolder : Holder {
    GC::Ref<GC::Cell> other_cell;
};
