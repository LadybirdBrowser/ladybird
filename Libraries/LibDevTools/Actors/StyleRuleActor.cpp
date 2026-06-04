/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <LibDevTools/Actors/StyleRuleActor.h>

namespace DevTools {

NonnullRefPtr<StyleRuleActor> StyleRuleActor::create(DevToolsServer& devtools, String name, JsonObject rule)
{
    return adopt_ref(*new StyleRuleActor(devtools, move(name), move(rule)));
}

StyleRuleActor::StyleRuleActor(DevToolsServer& devtools, String name, JsonObject rule)
    : Actor(devtools, move(name))
    , m_rule(move(rule))
{
    m_rule.set("actor"sv, this->name());

    JsonObject traits;
    // FIXME: Support modifying style rules inside the inspector.
    traits.set("canSetRuleText"sv, false);
    m_rule.set("traits"sv, move(traits));
    m_rule.set("ancestorData"sv, JsonArray {});
}

StyleRuleActor::~StyleRuleActor() = default;

JsonObject StyleRuleActor::serialize_rule() const
{
    return m_rule;
}

void StyleRuleActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "getRuleText"sv) {
        response.set("text"sv, m_rule.get_string("cssText"sv).value_or({}));
        send_response(message, move(response));
        return;
    }

    if (message.type == "modifyProperties"sv || message.type == "setRuleText"sv || message.type == "modifySelector"sv) {
        response.set("error"sv, "unsupported"sv);
        response.set("message"sv, "Live CSS rule editing is not supported"sv);
        send_response(message, move(response));
        return;
    }

    if (message.type == "getQueryContainerForNode"sv) {
        response.set("container"sv, JsonValue {});
        send_response(message, move(response));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

}
