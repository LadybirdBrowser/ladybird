/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Scripting/EnvironmentSettingsSnapshot.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(EnvironmentSettingsSnapshot);

EnvironmentSettingsSnapshot::EnvironmentSettingsSnapshot(NonnullOwnPtr<JS::ExecutionContext> execution_context, SerializedEnvironmentSettingsObject const& serialized_settings)
    : EnvironmentSettingsObject(move(execution_context))
    , m_api_url_character_encoding(serialized_settings.api_url_character_encoding)
    , m_url(serialized_settings.api_base_url)
    , m_origin(serialized_settings.origin)
    , m_policy_container(serialized_settings.policy_container)
{
    // Why can't we put these in the init list? grandparent class members are strange it seems
    this->id = serialized_settings.id;
    this->creation_url = serialized_settings.creation_url;
    this->top_level_creation_url = serialized_settings.top_level_creation_url;
    this->top_level_creation_url = serialized_settings.top_level_creation_url;
}

// Out of line to ensure this class has a key function
EnvironmentSettingsSnapshot::~EnvironmentSettingsSnapshot() = default;

}
