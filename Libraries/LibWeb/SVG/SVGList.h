/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::SVG {

// https://www.w3.org/TR/SVG2/types.html#ReadOnlyList
enum class ReadOnlyList : u8 {
    Yes,
    No,
};

// https://www.w3.org/TR/SVG2/types.html#TermListInterface
template<typename T>
class SVGList {
public:
    // https://www.w3.org/TR/SVG2/types.html#__svg__SVGNameList__length
    WebIDL::UnsignedLong length() const;
    WebIDL::UnsignedLong number_of_items() const { return length(); }

    WebIDL::ExceptionOr<void> clear();
    WebIDL::ExceptionOr<T> initialize_(T);
    WebIDL::ExceptionOr<T> get_item(WebIDL::UnsignedLong);
    WebIDL::ExceptionOr<T> insert_item_before(T, WebIDL::UnsignedLong);
    WebIDL::ExceptionOr<T> replace_item(T, WebIDL::UnsignedLong);
    WebIDL::ExceptionOr<T> remove_item(WebIDL::UnsignedLong);
    WebIDL::ExceptionOr<T> append_item(T);

    ReadonlySpan<T> items() { return m_items; }

protected:
    SVGList(JS::Realm&, Vector<T>, ReadOnlyList);
    SVGList(JS::Realm&, ReadOnlyList);

    void visit_edges(GC::Cell::Visitor& visitor);

    ReadOnlyList read_only() const { return m_read_only; }

private:
    GC::Ref<JS::Realm> m_realm;
    Vector<T> m_items;

    // https://www.w3.org/TR/SVG2/types.html#ReadOnlyList
    ReadOnlyList m_read_only;
};

}
