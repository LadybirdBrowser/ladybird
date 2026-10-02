/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <LibGC/Ptr.h>
#include <LibURL/Origin.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/RemoteEnvironmentSettings.h>

namespace Web::HTML {

// A fetch client: a settings object of this process, or the settings of one another process hosts.
using FetchClient = Variant<GC::Ref<EnvironmentSettingsObject>, NonnullRefPtr<RemoteEnvironmentSettings const>>;

WEB_API URL::Origin origin_of_fetch_client(FetchClient const&);
WEB_API GC::Ptr<EnvironmentSettingsObject> settings_object_of_fetch_client(Optional<FetchClient> const&);

}
