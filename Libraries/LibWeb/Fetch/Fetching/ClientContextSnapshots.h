/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/ClientContextSnapshot.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Fetching {

[[nodiscard]] WEB_API GC::Ptr<HTML::EnvironmentSettingsObject> resolve_client(Infrastructure::Request const&);

[[nodiscard]] WEB_API NonnullRefPtr<Infrastructure::ClientContextSnapshot const> snapshot_client_context(HTML::EnvironmentSettingsObject&);
[[nodiscard]] WEB_API NonnullRefPtr<Infrastructure::ReservedClientContextSnapshot const> snapshot_reserved_client_context(HTML::Environment const&);

}
