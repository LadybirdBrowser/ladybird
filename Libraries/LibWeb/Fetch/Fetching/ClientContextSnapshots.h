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
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/FetchClient.h>

namespace Web::Fetch::Fetching {

// Request's client when it is a settings object of this process.
[[nodiscard]] WEB_API GC::Ptr<HTML::EnvironmentSettingsObject> resolve_client(Infrastructure::Request const&);

// Request's client when another process hosts it.
[[nodiscard]] WEB_API RefPtr<HTML::RemoteEnvironmentSettings const> remote_client(Infrastructure::Request const&);

[[nodiscard]] WEB_API Optional<HTML::FetchClient> resolve_fetch_client(Infrastructure::Request const&);

[[nodiscard]] WEB_API Infrastructure::Request::ClientType request_client(HTML::FetchClient const&);

[[nodiscard]] WEB_API NonnullRefPtr<Infrastructure::ClientContextSnapshot const> snapshot_client_context(HTML::EnvironmentSettingsObject&);
[[nodiscard]] WEB_API NonnullRefPtr<Infrastructure::ClientContextSnapshot const> snapshot_client_context(HTML::RemoteEnvironmentSettings const&);
[[nodiscard]] WEB_API NonnullRefPtr<Infrastructure::ReservedClientContextSnapshot const> snapshot_reserved_client_context(HTML::Environment const&);

}
