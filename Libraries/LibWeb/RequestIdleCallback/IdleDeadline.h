/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/IdleDeadline.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::RequestIdleCallback {

class IdleDeadline final : public Bindings::Wrappable {
    WEB_WRAPPABLE(IdleDeadline, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(IdleDeadline);

public:
    [[nodiscard]] static GC::Ref<IdleDeadline> create(JS::Realm&, bool did_timeout = false);
    virtual ~IdleDeadline() override;

    double time_remaining() const;
    bool did_timeout() const { return m_did_timeout; }

private:
    IdleDeadline(JS::Realm&, bool did_timeout);

    bool m_did_timeout { false };
};

}
