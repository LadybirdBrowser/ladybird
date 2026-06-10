/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class UserActivation final : public Bindings::Wrappable {
    WEB_WRAPPABLE(UserActivation, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(UserActivation);

public:
    [[nodiscard]] static GC::Ref<UserActivation> create(Window&);
    virtual ~UserActivation() override = default;

    bool has_been_active() const;
    bool is_active() const;

private:
    explicit UserActivation(Window&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<Window> m_window;
};

}
