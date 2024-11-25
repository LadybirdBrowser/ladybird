/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Scripting/EnvironmentSettingsSnapshot.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(EnvironmentSettingsSnapshot);

EnvironmentSettingsSnapshot::EnvironmentSettingsSnapshot(JS::Realm& realm, NonnullOwnPtr<JS::ExecutionContext> execution_context, SerializedEnvironmentSettingsObject const& serialized_settings)
    : EnvironmentSettingsObject(move(execution_context))
    , m_api_url_character_encoding(serialized_settings.api_url_character_encoding)
    , m_url(serialized_settings.api_base_url)
    , m_origin(serialized_settings.origin)
    , m_policy_container(create_a_policy_container_from_serialized_policy_container(realm, serialized_settings.policy_container))
    , m_time_origin(serialized_settings.time_origin)
{
    // Why can't we put these in the init list? grandparent class members are strange it seems
    this->id = serialized_settings.id;
    this->creation_url = serialized_settings.creation_url;
    this->top_level_creation_url = serialized_settings.top_level_creation_url;
}

// Out of line to ensure this class has a key function
EnvironmentSettingsSnapshot::~EnvironmentSettingsSnapshot() = default;

void EnvironmentSettingsSnapshot::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_policy_container);
}

}
