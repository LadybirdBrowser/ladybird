/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibJS/Forward.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::DOM {

class IDLEventListener final : public GC::Cell {
    GC_CELL(IDLEventListener, GC::Cell);
    GC_DECLARE_ALLOCATOR(IDLEventListener);

public:
    [[nodiscard]] static GC::Ref<IDLEventListener> create(GC::Ref<WebIDL::CallbackType>);

    virtual ~IDLEventListener() = default;

    WebIDL::CallbackType& callback() { return *m_callback; }

private:
    explicit IDLEventListener(GC::Ref<WebIDL::CallbackType>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<WebIDL::CallbackType> m_callback;
};

}
