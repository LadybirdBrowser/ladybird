/*
 * Copyright (c) 2022, Ben Abraham <ben.d.abraham@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class WorkerEnvironmentSettingsObject final
    : public EnvironmentSettingsObject {
    JS_CELL(WorkerEnvironmentSettingsObject, EnvironmentSettingsObject);
    JS_DECLARE_ALLOCATOR(WorkerEnvironmentSettingsObject);

public:
    WorkerEnvironmentSettingsObject(NonnullOwnPtr<JS::ExecutionContext> execution_context, JS::NonnullGCPtr<WorkerGlobalScope> global_scope)
        : EnvironmentSettingsObject(move(execution_context))
        , m_global_scope(global_scope)
    {
    }

    static JS::NonnullGCPtr<WorkerEnvironmentSettingsObject> setup(JS::NonnullGCPtr<Page> page, NonnullOwnPtr<JS::ExecutionContext> execution_context, SerializedEnvironmentSettingsObject const& outside_settings, HighResolutionTime::DOMHighResTimeStamp unsafe_worker_creation_time);

    virtual ~WorkerEnvironmentSettingsObject() override = default;

    JS::GCPtr<DOM::Document> responsible_document() override { return nullptr; }
    String api_url_character_encoding() override { return m_api_url_character_encoding; }
    URL::URL api_base_url() override;
    URL::Origin origin() override;
    PolicyContainer policy_container() override;
    CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() override;

private:
    virtual void visit_edges(JS::Cell::Visitor&) override;

    String m_api_url_character_encoding;
    URL::Origin m_origin;

    JS::NonnullGCPtr<WorkerGlobalScope> m_global_scope;
};

}
