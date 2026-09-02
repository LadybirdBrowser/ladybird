/*
 * Copyright (c) 2026-present, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/NavigateParams.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/document-sequences.html#navigable
class WEB_API Navigable : public JS::Cell {
    GC_CELL(Navigable, JS::Cell);

public:
    virtual ~Navigable() override;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-id
    CrossProcessId id() const { return m_id; }

    GC::Ptr<Navigable> parent() const { return m_parent; }

    bool is_ancestor_of(Navigable const&) const;

    virtual GC::Ptr<WindowProxy> active_window_proxy() = 0;
    virtual Utf16String const& target_name() const = 0;
    GC::Ref<Navigable> top_level_traversable();
    virtual bool is_top_level_traversable() const { return false; }
    virtual Optional<URL::URL> active_document_url() const = 0;
    virtual Optional<URL::Origin> active_document_origin() const = 0;

    virtual WebIDL::ExceptionOr<void> navigate(NavigateParams) = 0;

    bool allowed_by_sandboxing_to_navigate(Navigable const& target, SourceSnapshotParams const&) const;

protected:
    Navigable() = default;
    void set_id(CrossProcessId id) { m_id = id; }
    void set_parent(GC::Ptr<Navigable> parent) { m_parent = parent; }

    virtual void visit_edges(Cell::Visitor&) override;

private:
    CrossProcessId m_id;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-parent
    GC::Ptr<Navigable> m_parent;
};

}
