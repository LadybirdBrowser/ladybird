/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <LibGfx/Cursor.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/Export.h>

namespace Web {

// The cursor a page asks its client to show. The main thread asks for one as it handles the pointer, and so does a hover
// of the render clock while a task runs, on the StyleLayout thread. Each asks under one lock, so the client is left
// showing the cursor asked for last.
class WEB_API PageCursor final : public AtomicRefCounted<PageCursor> {
public:
    // Asks the client to show a cursor, from any thread.
    using Send = Function<void(Gfx::Cursor const&)>;

    static NonnullRefPtr<PageCursor> create() { return adopt_ref(*new PageCursor); }

    Gfx::Cursor current() const;

    // Asks the client to show `cursor`, where the cursor asked for last is another.
    void request(Gfx::Cursor const&);

    // What asks the client from now on. None asks nothing.
    void set_send(Send);

private:
    PageCursor() = default;

    mutable Mutex m_mutex;
    Gfx::Cursor m_current { Gfx::StandardCursor::Arrow };
    Send m_send;
};

Gfx::Cursor css_to_gfx_cursor(CSS::CursorPredefined);

}
