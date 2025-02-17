/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/TargetConfigurationActor.h>

namespace DevTools {

NonnullRefPtr<TargetConfigurationActor> TargetConfigurationActor::create(DevToolsServer& devtools, ByteString name)
{
    return adopt_ref(*new TargetConfigurationActor(devtools, move(name)));
}

TargetConfigurationActor::TargetConfigurationActor(DevToolsServer& devtools, ByteString name)
    : Actor(devtools, move(name))
{
}

TargetConfigurationActor::~TargetConfigurationActor() = default;

void TargetConfigurationActor::handle_message(StringView type, JsonObject const& message)
{
    JsonObject response;
    response.set("from"sv, name());

    if (type == "updateConfiguration"sv) {
        auto configuration = message.get_object("configuration"sv);
        if (!configuration.has_value()) {
            send_missing_parameter_error("configuration"sv);
            return;
        }

        send_message(move(response));
        return;
    }

    send_unrecognized_packet_type_error(type);
}

JsonObject TargetConfigurationActor::serialize_configuration() const
{
    JsonObject supported_options;
    supported_options.set("cacheDisabled"sv, false);
    supported_options.set("colorSchemeSimulation"sv, false);
    supported_options.set("customFormatters"sv, false);
    supported_options.set("customUserAgent"sv, false);
    supported_options.set("javascriptEnabled"sv, false);
    supported_options.set("overrideDPPX"sv, false);
    supported_options.set("printSimulationEnabled"sv, false);
    supported_options.set("rdmPaneMaxTouchPoints"sv, false);
    supported_options.set("rdmPaneOrientation"sv, false);
    supported_options.set("recordAllocations"sv, false);
    supported_options.set("reloadOnTouchSimulationToggle"sv, false);
    supported_options.set("restoreFocus"sv, false);
    supported_options.set("serviceWorkersTestingEnabled"sv, false);
    supported_options.set("setTabOffline"sv, false);
    supported_options.set("touchEventsOverride"sv, false);
    supported_options.set("tracerOptions"sv, false);
    supported_options.set("useSimpleHighlightersForReducedMotion"sv, false);

    JsonObject traits;
    traits.set("supportedOptions"sv, move(supported_options));

    JsonObject target;
    target.set("actor"sv, name());
    target.set("configuration"sv, JsonObject {});
    target.set("traits"sv, move(traits));

    return target;
}

}
