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
#include <AK/Optional.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>

namespace Web {

// The link a page asks its client to show the URL of, as the pointer hovers it. No script takes part in this, so what
// is hovered may be told from any thread. Each asks under one lock, so the client is left showing the link asked for
// last.
class WEB_API PageHoverLink final : public AtomicRefCounted<PageHoverLink> {
public:
    // Asks the client to show the URL of a link, or none, from any thread.
    using Send = Function<void(Optional<URL::URL> const&)>;

    static NonnullRefPtr<PageHoverLink> create() { return adopt_ref(*new PageHoverLink); }

    Optional<URL::URL> current() const;
    bool has_link() const;

    // Asks the client to show `url`, or no link, where the link asked for last is another.
    void request(Optional<URL::URL> url);

    // What asks the client from now on. None asks nothing.
    void set_send(Send);

private:
    PageHoverLink() = default;

    mutable Mutex m_mutex;
    Optional<URL::URL> m_current;
    Send m_send;
};

}
