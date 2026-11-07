/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify -ferror-limit=0 %plugin_opts% -c %s -o %t 2>&1

#include <AK/Function.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Forward.h>
#include <LibGC/Member.h>
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

struct HoldsCallback {
    GC_HOLDS_NO_CELL_EDGES;

    // expected-error@+1 {{HoldsCallback is declared to hold no cell edges, but its member callback holds an AK::Function, whose captures can't be checked}}
    Function<void()> callback;
};

struct HoldsCallbacks {
    GC_HOLDS_NO_CELL_EDGES;

    // expected-error@+1 {{HoldsCallbacks is declared to hold no cell edges, but its member callbacks holds an AK::Function, whose captures can't be checked}}
    Vector<Function<void()>> callbacks;
};

struct KeepsCallbacksWithRoots {
    Function<void()> callback;
    // expected-error@+1 {{KeepsCallbacksWithRoots is declared to hold no cell edges, but its member cell holds a GC pointer}}
    GC::Ptr<GC::Cell> cell;
};

GC_UNTRACED_CAPTURE_WITH_ROOTED_CALLBACKS(KeepsCallbacksWithRoots);

struct OwnsCell {
    // expected-error@+1 {{OwnsCell is declared to hold no cell edges, but its member inner holds a GC pointer}}
    OwnPtr<Inner> inner;
};

GC_UNTRACED_CAPTURE(OwnsCell);

struct CountedCell : RefCounted<CountedCell> {
    GC::Ptr<GC::Cell> cell;
};

struct SharesCell {
    GC_HOLDS_NO_CELL_EDGES;

    // expected-error@+1 {{SharesCell is declared to hold no cell edges, but its member counted holds a GC pointer}}
    RefPtr<CountedCell> counted;
};

struct CountedPlainData : RefCounted<CountedPlainData> {
    GC_HOLDS_NO_CELL_EDGES;

    int value { 0 };
};

struct SharesPlainData {
    GC_HOLDS_NO_CELL_EDGES;

    RefPtr<CountedPlainData> counted;
};

struct HoldsRawCell {
    GC_HOLDS_NO_CELL_EDGES;

    // expected-error@+1 {{HoldsRawCell is declared to hold no cell edges, but its member cell holds a GC pointer}}
    GC::RawPtr<GC::Cell> cell;
};

struct HoldsMemberCell {
    GC_HOLDS_NO_CELL_EDGES;

    // expected-error@+1 {{HoldsMemberCell is declared to hold no cell edges, but its member cell holds a GC pointer}}
    GC::Member<GC::Cell> cell;
};

struct InheritsCallback {
    Function<void()> callback;
};

// expected-error@+1 {{DeclaresOverBaseWithCallback is declared to hold no cell edges, but its base 'InheritsCallback' holds an AK::Function, whose captures can't be checked}}
struct DeclaresOverBaseWithCallback : InheritsCallback {
    GC_HOLDS_NO_CELL_EDGES;
};

// An owned object's callbacks are its own API.
struct OwnsCallbackHolder {
    GC_HOLDS_NO_CELL_EDGES;

    OwnPtr<InheritsCallback> holder;
};

struct CountedAndClaimed : RefCounted<CountedAndClaimed> {
    GC_HOLDS_NO_CELL_EDGES;

    // expected-error@+1 {{CountedAndClaimed is declared to hold no cell edges, but its member cell holds a GC pointer}}
    GC::Ptr<GC::Cell> cell;
};

template<typename T>
struct ClaimedTemplate {
    GC_HOLDS_NO_CELL_EDGES;

    // expected-error@+1 {{ClaimedTemplate<GC::Ref<GC::Cell>> is declared to hold no cell edges, but its member value holds a GC pointer}}
    T value;
};

inline ClaimedTemplate<int> plain_instance;
inline ClaimedTemplate<GC::Ref<GC::Cell>>* cell_instance_pointer;
inline auto cell_instance_size = sizeof(ClaimedTemplate<GC::Ref<GC::Cell>>);

struct OwnedBase {
    virtual ~OwnedBase() = default;
};

struct OwnedSubclassWithCell : OwnedBase {
    // expected-error@+1 {{Member cell of polymorphic class OwnedSubclassWithCell contains GC pointers but OwnedSubclassWithCell has no visit_edges method}}
    GC::Ptr<GC::Cell> cell;
};

struct OwnsPolymorphicBase {
    GC_HOLDS_NO_CELL_EDGES;

    // expected-error@+1 {{OwnsPolymorphicBase is declared to hold no cell edges, but its member owned holds a GC pointer}}
    OwnPtr<OwnedBase> owned;
};

struct ClaimedPolymorphicBase {
    virtual ~ClaimedPolymorphicBase() = default;
    int value { 0 };
};
GC_UNTRACED_CAPTURE(ClaimedPolymorphicBase);

struct PlainSubclassOfClaimedBase : ClaimedPolymorphicBase {
    int more { 0 };
};

struct SubclassOfClaimedBaseWithCell : ClaimedPolymorphicBase {
    // expected-error@+2 {{Member cell of polymorphic class SubclassOfClaimedBaseWithCell contains GC pointers but SubclassOfClaimedBaseWithCell has no visit_edges method}}
    // expected-error@+1 {{SubclassOfClaimedBaseWithCell (through its base ClaimedPolymorphicBase) is declared to hold no cell edges, but its member cell holds a GC pointer}}
    GC::Ptr<GC::Cell> cell;
};

struct IndirectSubclassOfClaimedBaseWithCallback : PlainSubclassOfClaimedBase {
    // expected-error@+1 {{IndirectSubclassOfClaimedBaseWithCallback (through its base ClaimedPolymorphicBase) is declared to hold no cell edges, but its member callback holds an AK::Function, whose captures can't be checked}}
    Function<void()> callback;
};

struct MixinOfCell {
    virtual ~MixinOfCell() = default;
    void visit_edges(GC::Cell::Visitor&) { }
};

struct PointsToMixin {
    GC_HOLDS_NO_CELL_EDGES;

    // expected-error@+1 {{PointsToMixin is declared to hold no cell edges, but its member mixin holds a GC pointer}}
    MixinOfCell* mixin { nullptr };
};
