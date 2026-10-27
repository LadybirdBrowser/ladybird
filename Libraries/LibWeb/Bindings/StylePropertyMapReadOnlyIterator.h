/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/CSS/StylePropertyMapReadOnly.h>

namespace Web::CSS {

class StylePropertyMapReadOnlyIterator final : public JS::Cell {
    GC_CELL(StylePropertyMapReadOnlyIterator, JS::Cell);
    GC_DECLARE_ALLOCATOR(StylePropertyMapReadOnlyIterator);

public:
    using JSValueConversionIsForbidden = void;

    static GC::Ref<JS::HostObject> create(JS::Realm&, StylePropertyMapReadOnly&, JS::Object::PropertyKind);
    GC::Ref<JS::Object> next(JS::Realm&);

private:
    StylePropertyMapReadOnlyIterator(JS::Object::PropertyKind, Vector<StylePropertyMapReadOnly::IterationEntry>, ReadonlySpan<GC::Ref<CSSStyleValue>>);
    virtual void visit_edges(GC::Cell::Visitor&) override;

    JS::Object::PropertyKind m_kind;
    Vector<StylePropertyMapReadOnly::IterationEntry> m_entries;
    Vector<GC::Ref<CSSStyleValue>> m_values;
    size_t m_index { 0 };
};

}
