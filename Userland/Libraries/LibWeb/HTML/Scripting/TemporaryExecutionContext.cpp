/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>

namespace Web::HTML {

TemporaryExecutionContext::TemporaryExecutionContext(JS::Realm& realm, CallbacksEnabled callbacks_enabled)
    : m_realm(realm)
    , m_callbacks_enabled(callbacks_enabled)
{
    prepare_to_run_script(m_realm);
    if (m_callbacks_enabled == CallbacksEnabled::Yes)
        prepare_to_run_callback(m_realm);
}

TemporaryExecutionContext::~TemporaryExecutionContext()
{
    clean_up_after_running_script(m_realm);
    if (m_callbacks_enabled == CallbacksEnabled::Yes)
        clean_up_after_running_callback(m_realm);
}

}
