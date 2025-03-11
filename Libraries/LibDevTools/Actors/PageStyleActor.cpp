/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibDevTools/Actors/InspectorActor.h>
#include <LibDevTools/Actors/PageStyleActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/WalkerActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<PageStyleActor> PageStyleActor::create(DevToolsServer& devtools, String name, WeakPtr<InspectorActor> inspector)
{
    return adopt_ref(*new PageStyleActor(devtools, move(name), move(inspector)));
}

PageStyleActor::PageStyleActor(DevToolsServer& devtools, String name, WeakPtr<InspectorActor> inspector)
    : Actor(devtools, move(name))
    , m_inspector(move(inspector))
{
}

PageStyleActor::~PageStyleActor() = default;

void PageStyleActor::handle_message(StringView type, JsonObject const& message)
{
    JsonObject response;

    if (type == "getApplied"sv) {
        // FIXME: This provides information to the "styles" pane in the inspector tab, which allows toggling and editing
        //        styles live. We do not yet support figuring out the list of styles that apply to a specific node.
        response.set("entries"sv, JsonArray {});
        send_message(move(response));
        return;
    }

    if (type == "getComputed"sv) {
        auto node = message.get_string("node"sv);
        if (!node.has_value()) {
            send_missing_parameter_error("node"sv);
            return;
        }

        inspect_dom_node(*node, [](auto& self, auto const& properties, auto block_token) {
            self.received_computed_style(properties.computed_style, move(block_token));
        });

        return;
    }

    if (type == "getLayout"sv) {
        auto node = message.get_string("node"sv);
        if (!node.has_value()) {
            send_missing_parameter_error("node"sv);
            return;
        }

        inspect_dom_node(*node, [](auto& self, auto const& properties, auto block_token) {
            self.received_layout(properties.computed_style, properties.node_box_sizing, move(block_token));
        });

        return;
    }

    if (type == "isPositionEditable") {
        response.set("value"sv, false);
        send_message(move(response));
        return;
    }

    send_unrecognized_packet_type_error(type);
}

JsonValue PageStyleActor::serialize_style() const
{
    JsonObject traits;
    traits.set("fontStyleLevel4"sv, true);
    traits.set("fontWeightLevel4"sv, true);
    traits.set("fontStretchLevel4"sv, true);
    traits.set("fontVariations"sv, true);

    JsonObject style;
    style.set("actor"sv, name());
    style.set("traits"sv, move(traits));
    return style;
}

template<typename Callback>
void PageStyleActor::inspect_dom_node(StringView node_actor, Callback&& callback)
{
    auto dom_node = WalkerActor::dom_node_for(InspectorActor::walker_for(m_inspector), node_actor);
    if (!dom_node.has_value()) {
        send_unknown_actor_error(node_actor);
        return;
    }

    auto block_token = block_responses();

    devtools().delegate().inspect_dom_node(
        dom_node->tab->description(), dom_node->identifier.id, dom_node->identifier.pseudo_element,
        [weak_self = make_weak_ptr<PageStyleActor>(), block_token = move(block_token), callback = forward<Callback>(callback)](ErrorOr<DOMNodeProperties> properties) mutable {
            if (properties.is_error()) {
                dbgln_if(DEVTOOLS_DEBUG, "Unable to inspect DOM node: {}", properties.error());
                return;
            }

            if (auto self = weak_self.strong_ref())
                callback(*self, properties.value(), move(block_token));
        });
}

void PageStyleActor::received_layout(JsonObject const& computed_style, JsonObject const& node_box_sizing, BlockToken block_token)
{
    JsonObject message;
    message.set("autoMargins"sv, JsonObject {});

    auto pixel_value = [&](auto const& object, auto key) {
        return object.get_double_with_precision_loss(key).value_or(0);
    };
    auto set_pixel_value_from = [&](auto const& object, auto object_key, auto message_key) {
        message.set(message_key, MUST(String::formatted("{}px", pixel_value(object, object_key))));
    };
    auto set_computed_value_from = [&](auto const& object, auto key) {
        message.set(key, object.get_string(key).value_or(String {}));
    };

    message.set("width"sv, pixel_value(node_box_sizing, "content_width"sv));
    message.set("height"sv, pixel_value(node_box_sizing, "content_height"sv));

    // FIXME: This response should also contain "top", "right", "bottom", and "left", but our box model metrics in
    //        WebContent do not provide this information.

    set_pixel_value_from(node_box_sizing, "border_top"sv, "border-top-width"sv);
    set_pixel_value_from(node_box_sizing, "border_right"sv, "border-right-width"sv);
    set_pixel_value_from(node_box_sizing, "border_bottom"sv, "border-bottom-width"sv);
    set_pixel_value_from(node_box_sizing, "border_left"sv, "border-left-width"sv);

    set_pixel_value_from(node_box_sizing, "margin_top"sv, "margin-top"sv);
    set_pixel_value_from(node_box_sizing, "margin_right"sv, "margin-right"sv);
    set_pixel_value_from(node_box_sizing, "margin_bottom"sv, "margin-bottom"sv);
    set_pixel_value_from(node_box_sizing, "margin_left"sv, "margin-left"sv);

    set_pixel_value_from(node_box_sizing, "padding_top"sv, "padding-top"sv);
    set_pixel_value_from(node_box_sizing, "padding_right"sv, "padding-right"sv);
    set_pixel_value_from(node_box_sizing, "padding_bottom"sv, "padding-bottom"sv);
    set_pixel_value_from(node_box_sizing, "padding_left"sv, "padding-left"sv);

    set_computed_value_from(computed_style, "box-sizing"sv);
    set_computed_value_from(computed_style, "display"sv);
    set_computed_value_from(computed_style, "float"sv);
    set_computed_value_from(computed_style, "line-height"sv);
    set_computed_value_from(computed_style, "position"sv);
    set_computed_value_from(computed_style, "z-index"sv);

    send_message(move(message), move(block_token));
}

void PageStyleActor::received_computed_style(JsonObject const& computed_style, BlockToken block_token)
{
    JsonObject computed;

    computed_style.for_each_member([&](String const& name, JsonValue const& value) {
        JsonObject property;
        property.set("matched"sv, true);
        property.set("value"sv, value);
        computed.set(name, move(property));
    });

    JsonObject message;
    message.set("computed"sv, move(computed));
    send_message(move(message), move(block_token));
}

}
