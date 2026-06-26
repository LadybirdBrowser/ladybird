/*
 * Copyright (c) 2026-present, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibJS/Heap/Cell.h>
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

protected:
    Navigable() = default;
    void set_id(String id) { m_id = move(id); }

private:
    String m_id;
};

}
