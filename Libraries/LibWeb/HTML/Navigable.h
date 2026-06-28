/*
 * Copyright (c) 2026-present, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/document-sequences.html#navigable
class WEB_API Navigable : public JS::Cell {
    GC_CELL(Navigable, JS::Cell);

public:
    virtual ~Navigable() override;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-id
    String const& id() const { return m_id; }

    GC::Ptr<Navigable> parent() const { return m_parent; }

    bool is_ancestor_of(GC::Ref<Navigable>) const;

    virtual GC::Ptr<WindowProxy> active_window_proxy() = 0;
    virtual String target_name() const = 0;
    GC::Ref<Navigable> top_level_traversable();
    virtual Optional<URL::URL> active_document_url() const = 0;
    virtual Optional<URL::Origin> active_document_origin() const = 0;

protected:
    Navigable() = default;
    void set_id(String id) { m_id = move(id); }
    void set_parent(GC::Ptr<Navigable> parent) { m_parent = parent; }

    virtual void visit_edges(Cell::Visitor&) override;

private:
    String m_id;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-parent
    GC::Ptr<Navigable> m_parent;
};

}
