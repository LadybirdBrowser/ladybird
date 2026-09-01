/*
 * Copyright (c) 2022-2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Variant.h>
#include <LibGC/Function.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/Scripting/ModuleScript.h>

namespace Web::HTML {

class ModuleLocationTuple {
public:
    ModuleLocationTuple(URL::URL url, Utf16View type)
        : m_url(move(url))
        , m_type(Utf16String::from_utf16(type))
    {
    }

    URL::URL const& url() const { return m_url; }
    Utf16String const& type() const { return m_type; }

    bool operator==(ModuleLocationTuple const& other) const
    {
        return other.url() == m_url && other.type() == m_type;
    }

private:
    URL::URL m_url;
    Utf16String m_type;
};

// https://html.spec.whatwg.org/multipage/webappapis.html#module-map
class WEB_API ModuleMap final : public JS::Cell {
    GC_CELL(ModuleMap, JS::Cell);
    GC_DECLARE_ALLOCATOR(ModuleMap);

public:
    ModuleMap() = default;
    ~ModuleMap() = default;

    using CallbackFunction = GC::Ref<GC::Function<void(GC::Ptr<ModuleScript>)>>;
    using CallbackList = Vector<CallbackFunction>;

    // A module map is a map keyed by tuples consisting of a URL record and a string. The URL record is the request URL
    // at which the module was fetched, and the string indicates the type of the module (e.g. "javascript-or-wasm").
    // The module map's values are either a module script or a list of algorithms (that are waiting for the fetch to
    // complete).
    using Entry = Variant<GC::Ref<ModuleScript>, CallbackList>;

    Optional<Entry const&> get(URL::URL const& url, Utf16View type) const;

    void set(URL::URL const& url, Utf16View type, CallbackList);
    void append(URL::URL const& url, Utf16View type, CallbackFunction);
    void complete_fetch(URL::URL const& url, Utf16View type, GC::Ptr<ModuleScript>);

private:
    virtual void visit_edges(JS::Cell::Visitor&) override;

    HashMap<ModuleLocationTuple, Entry> m_values;
};

}

namespace AK {

template<>
struct Traits<Web::HTML::ModuleLocationTuple> : public DefaultTraits<Web::HTML::ModuleLocationTuple> {
    static unsigned hash(Web::HTML::ModuleLocationTuple const& tuple)
    {
        return pair_int_hash(tuple.url().to_byte_string().hash(), tuple.type().hash());
    }
};

}
