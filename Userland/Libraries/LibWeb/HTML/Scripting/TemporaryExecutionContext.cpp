/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>

namespace Web::HTML {

TemporaryExecutionContext::TemporaryExecutionContext(EnvironmentSettingsObject& environment_settings, CallbacksEnabled callbacks_enabled)
    : m_environment_settings(environment_settings)
    , m_callbacks_enabled(callbacks_enabled)
{
    prepare_to_run_script(m_environment_settings->realm());
    if (m_callbacks_enabled == CallbacksEnabled::Yes)
        m_environment_settings->prepare_to_run_callback();
}

TemporaryExecutionContext::~TemporaryExecutionContext()
{
    clean_up_after_running_script(m_environment_settings->realm());
    if (m_callbacks_enabled == CallbacksEnabled::Yes)
        m_environment_settings->clean_up_after_running_callback();
}

}
