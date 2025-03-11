/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonValue.h>
#include <LibDevTools/Actors/InspectorActor.h>
#include <LibDevTools/Actors/PageStyleActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/WalkerActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

static void received_layout(JsonObject& response, JsonObject const& computed_style, JsonObject const& node_box_sizing)
{
    response.set("autoMargins"sv, JsonObject {});

    auto pixel_value = [&](auto const& object, auto key) {
        return object.get_double_with_precision_loss(key).value_or(0);
    };
    auto set_pixel_value_from = [&](auto const& object, auto object_key, auto message_key) {
        response.set(message_key, MUST(String::formatted("{}px", pixel_value(object, object_key))));
    };
    auto set_computed_value_from = [&](auto const& object, auto key) {
        response.set(key, object.get_string(key).value_or(String {}));
    };

    response.set("width"sv, pixel_value(node_box_sizing, "content_width"sv));
    response.set("height"sv, pixel_value(node_box_sizing, "content_height"sv));

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
}

static void received_computed_style(JsonObject& response, JsonObject const& computed_style)
{
    JsonObject computed;

    computed_style.for_each_member([&](String const& name, JsonValue const& value) {
        JsonObject property;
        property.set("matched"sv, true);
        property.set("value"sv, value);
        computed.set(name, move(property));
    });

    response.set("computed"sv, move(computed));
}

static void received_fonts(JsonObject& response, JsonArray const& fonts)
{
    JsonArray font_faces;

    fonts.for_each([&](JsonValue const& font) {
        if (!font.is_object())
            return;

        auto name = font.as_object().get_string("name"sv).value_or({});
        auto weight = font.as_object().get_integer<i64>("weight"sv).value_or(0);

        JsonObject font_face;
        font_face.set("CSSFamilyName"sv, name);
        font_face.set("CSSGeneric"sv, JsonValue {});
        font_face.set("format"sv, ""sv);
        font_face.set("localName"sv, ""sv);
        font_face.set("metadata"sv, ""sv);
        font_face.set("name"sv, name);
        font_face.set("srcIndex"sv, -1);
        font_face.set("style"sv, ""sv);
        font_face.set("URI"sv, ""sv);
        font_face.set("variationAxes"sv, JsonArray {});
        font_face.set("variationInstances"sv, JsonArray {});
        font_face.set("weight"sv, weight);

        font_faces.must_append(move(font_face));
    });

    response.set("fontFaces"sv, move(font_faces));
}

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

void PageStyleActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "getAllUsedFontFaces"sv) {
        response.set("fontFaces"sv, JsonArray {});
        send_response(message, move(response));
        return;
    }

    if (message.type == "getApplied"sv) {
        // FIXME: This provides information to the "styles" pane in the inspector tab, which allows toggling and editing
        //        styles live. We do not yet support figuring out the list of styles that apply to a specific node.
        response.set("entries"sv, JsonArray {});
        send_response(message, move(response));
        return;
    }

    if (message.type == "getComputed"sv) {
        auto node = get_required_parameter<String>(message, "node"sv);
        if (!node.has_value())
            return;

        inspect_dom_node(message, *node, [](auto& response, auto const& properties) {
            received_computed_style(response, properties.computed_style);
        });

        return;
    }

    if (message.type == "getLayout"sv) {
        auto node = get_required_parameter<String>(message, "node"sv);
        if (!node.has_value())
            return;

        inspect_dom_node(message, *node, [](auto& response, auto const& properties) {
            received_layout(response, properties.computed_style, properties.node_box_sizing);
        });

        return;
    }

    if (message.type == "getUsedFontFaces"sv) {
        auto node = get_required_parameter<String>(message, "node"sv);
        if (!node.has_value())
            return;

        inspect_dom_node(message, *node, [](auto& response, auto const& properties) {
            received_fonts(response, properties.fonts);
        });

        return;
    }

    if (message.type == "isPositionEditable") {
        response.set("value"sv, false);
        send_response(message, move(response));
        return;
    }

    send_unrecognized_packet_type_error(message);
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
void PageStyleActor::inspect_dom_node(Message const& message, StringView node_actor, Callback&& callback)
{
    auto dom_node = WalkerActor::dom_node_for(InspectorActor::walker_for(m_inspector), node_actor);
    if (!dom_node.has_value()) {
        send_unknown_actor_error(message, node_actor);
        return;
    }

    devtools().delegate().inspect_dom_node(dom_node->tab->description(), dom_node->identifier.id, dom_node->identifier.pseudo_element,
        async_handler(message, [callback = forward<Callback>(callback)](auto&, auto properties, auto& response) {
            callback(response, properties);
        }));
}

}
