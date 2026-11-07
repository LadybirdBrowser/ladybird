/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibGC/Cell.h>
#include <LibGC/Forward.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibGC/Weak.h>

struct HoldsPlainData {
    GC_HOLDS_NO_CELL_EDGES;

    int value { 0 };
    GC::Root<GC::Cell> root;
    GC::Weak<GC::Cell> weak;
};

struct HoldsCell {
    GC_HOLDS_NO_CELL_EDGES;

    // expected-error@+1 {{HoldsCell is declared to hold no cell edges, but its member cell holds a GC pointer}}
    GC::Ptr<GC::Cell> cell;
};

struct Inner {
    GC::Ref<GC::Cell> cell;
};

struct HoldsCellThroughMember {
    GC_HOLDS_NO_CELL_EDGES;

    // expected-error@+1 {{HoldsCellThroughMember is declared to hold no cell edges, but its member inner holds a GC pointer}}
    Inner inner;
};

struct Base {
    GC_HOLDS_NO_CELL_EDGES;
};

struct InheritsTheClaim : Base {
    // expected-error@+1 {{InheritsTheClaim is declared to hold no cell edges, but its member cell holds a GC pointer}}
    GC::Ptr<GC::Cell> cell;
};

struct InheritsCell {
    GC::Ptr<GC::Cell> cell;
};

// expected-error@+1 {{DeclaresOverBaseWithCell is declared to hold no cell edges, but its base 'InheritsCell' holds a GC pointer}}
struct DeclaresOverBaseWithCell : InheritsCell {
    GC_HOLDS_NO_CELL_EDGES;
};

struct DeclaredOutside {
    // expected-error@+1 {{DeclaredOutside is declared to hold no cell edges, but its member cell holds a GC pointer}}
    GC::Ptr<GC::Cell> cell;
};

GC_UNTRACED_CAPTURE(DeclaredOutside);

struct DeclaredOutsideWithPlainData {
    int value { 0 };
};

GC_UNTRACED_CAPTURE(DeclaredOutsideWithPlainData);
