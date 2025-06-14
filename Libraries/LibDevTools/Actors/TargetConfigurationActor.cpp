/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/TargetConfigurationActor.h>

namespace DevTools {

NonnullRefPtr<TargetConfigurationActor> TargetConfigurationActor::create(DevToolsServer& devtools, String name)
{
    return adopt_ref(*new TargetConfigurationActor(devtools, move(name)));
}

TargetConfigurationActor::TargetConfigurationActor(DevToolsServer& devtools, String name)
    : Actor(devtools, move(name))
{
}

TargetConfigurationActor::~TargetConfigurationActor() = default;

void TargetConfigurationActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "updateConfiguration"_sv) {
        auto configuration = get_required_parameter<JsonObject>(message, "configuration"_sv);
        if (!configuration.has_value())
            return;

        send_response(message, move(response));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

JsonObject TargetConfigurationActor::serialize_configuration() const
{
    JsonObject supported_options;
    supported_options.set("cacheDisabled"_sv, false);
    supported_options.set("colorSchemeSimulation"_sv, false);
    supported_options.set("customFormatters"_sv, false);
    supported_options.set("customUserAgent"_sv, false);
    supported_options.set("javascriptEnabled"_sv, false);
    supported_options.set("overrideDPPX"_sv, false);
    supported_options.set("printSimulationEnabled"_sv, false);
    supported_options.set("rdmPaneMaxTouchPoints"_sv, false);
    supported_options.set("rdmPaneOrientation"_sv, false);
    supported_options.set("recordAllocations"_sv, false);
    supported_options.set("reloadOnTouchSimulationToggle"_sv, false);
    supported_options.set("restoreFocus"_sv, false);
    supported_options.set("serviceWorkersTestingEnabled"_sv, false);
    supported_options.set("setTabOffline"_sv, false);
    supported_options.set("touchEventsOverride"_sv, false);
    supported_options.set("tracerOptions"_sv, false);
    supported_options.set("useSimpleHighlightersForReducedMotion"_sv, false);

    JsonObject traits;
    traits.set("supportedOptions"_sv, move(supported_options));

    JsonObject target;
    target.set("actor"_sv, name());
    target.set("configuration"_sv, JsonObject {});
    target.set("traits"_sv, move(traits));

    return target;
}

}
