/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>

namespace Web::StorageAPI {

class StorageManager final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(StorageManager, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(StorageManager);

public:
    static GC::Ref<StorageManager> create(GC::Ref<HTML::EnvironmentSettingsObject>);
    virtual ~StorageManager() override = default;

    GC::Ref<WebIDL::Promise> estimate() const;

private:
    explicit StorageManager(GC::Ref<HTML::EnvironmentSettingsObject>);
    static void queue_a_storage_task(JS::Object&, Function<void()>);
    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<HTML::EnvironmentSettingsObject> m_environment;
};

}
