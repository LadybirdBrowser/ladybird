/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/String.h>
#include <LibCore/Version.h>
#include <LibDevTools/Actors/DeviceActor.h>
#include <LibWeb/Loader/UserAgent.h>

namespace DevTools {

NonnullRefPtr<DeviceActor> DeviceActor::create(DevToolsServer& devtools, ByteString name)
{
    return adopt_ref(*new DeviceActor(devtools, move(name)));
}

DeviceActor::DeviceActor(DevToolsServer& devtools, ByteString name)
    : Actor(devtools, move(name))
{
}

DeviceActor::~DeviceActor() = default;

void DeviceActor::handle_message(StringView type, JsonObject const&)
{
    if (type == "getDescription"sv) {
        auto build_id = Core::Version::read_long_version_string().to_byte_string();

        static constexpr auto browser_name = StringView { BROWSER_NAME, __builtin_strlen(BROWSER_NAME) };
        static constexpr auto browser_version = StringView { BROWSER_VERSION, __builtin_strlen(BROWSER_VERSION) };
        static constexpr auto platform_name = StringView { OS_STRING, __builtin_strlen(OS_STRING) };
        static constexpr auto arch = StringView { CPU_STRING, __builtin_strlen(CPU_STRING) };

        // https://github.com/mozilla/gecko-dev/blob/master/devtools/shared/system.js
        JsonObject value;
        value.set("apptype"sv, browser_name.to_lowercase_string());
        value.set("name"sv, browser_name);
        value.set("brandName"sv, browser_name);
        value.set("version"sv, browser_version);
        value.set("appbuildid"sv, build_id);
        value.set("platformbuildid"sv, build_id);
        value.set("platformversion"sv, "135.0"sv);
        value.set("useragent"sv, Web::default_user_agent);
        value.set("os"sv, platform_name);
        value.set("arch"sv, arch);

        JsonObject response;
        response.set("from"sv, name());
        response.set("value"sv, move(value));

        send_message(move(response));
        return;
    }

    send_unrecognized_packet_type_error(type);
}

}
