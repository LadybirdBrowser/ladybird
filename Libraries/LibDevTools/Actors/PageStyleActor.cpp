/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibDevTools/Actors/InspectorActor.h>
#include <LibDevTools/Actors/PageStyleActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/WalkerActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

static void received_layout(JsonObject& response, JsonObject const& node_box_sizing)
{
    response.set("autoMargins"_sv, JsonObject {});

    auto pixel_value = [&](auto key) {
        return node_box_sizing.get_double_with_precision_loss(key).value_or(0);
    };
    auto set_pixel_value = [&](auto key) {
        response.set(key, MUST(String::formatted("{}px", pixel_value(key))));
    };
    auto set_computed_value = [&](auto key) {
        response.set(key, node_box_sizing.get_string(key).value_or(String {}));
    };

    // FIXME: This response should also contain "top", "right", "bottom", and "left", but our box model metrics in
    //        WebContent do not provide this information.

    set_computed_value("width"_sv);
    set_computed_value("height"_sv);

    set_pixel_value("border-top-width"_sv);
    set_pixel_value("border-right-width"_sv);
    set_pixel_value("border-bottom-width"_sv);
    set_pixel_value("border-left-width"_sv);

    set_pixel_value("margin-top"_sv);
    set_pixel_value("margin-right"_sv);
    set_pixel_value("margin-bottom"_sv);
    set_pixel_value("margin-left"_sv);

    set_pixel_value("padding-top"_sv);
    set_pixel_value("padding-right"_sv);
    set_pixel_value("padding-bottom"_sv);
    set_pixel_value("padding-left"_sv);

    set_computed_value("box-sizing"_sv);
    set_computed_value("display"_sv);
    set_computed_value("float"_sv);
    set_computed_value("line-height"_sv);
    set_computed_value("position"_sv);
    set_computed_value("z-index"_sv);
}

static void received_computed_style(JsonObject& response, JsonObject const& computed_style)
{
    JsonObject computed;

    computed_style.for_each_member([&](String const& name, JsonValue const& value) {
        JsonObject property;
        property.set("matched"_sv, true);
        property.set("value"_sv, value);
        computed.set(name, move(property));
    });

    response.set("computed"_sv, move(computed));
}

static void received_fonts(JsonObject& response, JsonArray const& fonts)
{
    JsonArray font_faces;

    fonts.for_each([&](JsonValue const& font) {
        if (!font.is_object())
            return;

        auto name = font.as_object().get_string("name"_sv).value_or({});
        auto weight = font.as_object().get_integer<i64>("weight"_sv).value_or(0);

        JsonObject font_face;
        font_face.set("CSSFamilyName"_sv, name);
        font_face.set("CSSGeneric"_sv, JsonValue {});
        font_face.set("format"_sv, ""_sv);
        font_face.set("localName"_sv, ""_sv);
        font_face.set("metadata"_sv, ""_sv);
        font_face.set("name"_sv, name);
        font_face.set("srcIndex"_sv, -1);
        font_face.set("style"_sv, ""_sv);
        font_face.set("URI"_sv, ""_sv);
        font_face.set("variationAxes"_sv, JsonArray {});
        font_face.set("variationInstances"_sv, JsonArray {});
        font_face.set("weight"_sv, weight);

        font_faces.must_append(move(font_face));
    });

    response.set("fontFaces"_sv, move(font_faces));
}

NonnullRefPtr<PageStyleActor> PageStyleActor::create(DevToolsServer& devtools, String name, WeakPtr<InspectorActor> inspector)
{
    return adopt_ref(*new PageStyleActor(devtools, move(name), move(inspector)));
}

PageStyleActor::PageStyleActor(DevToolsServer& devtools, String name, WeakPtr<InspectorActor> inspector)
    : Actor(devtools, move(name))
    , m_inspector(move(inspector))
{
    if (auto tab = InspectorActor::tab_for(m_inspector)) {
        devtools.delegate().listen_for_dom_properties(tab->description(),
            [weak_self = make_weak_ptr<PageStyleActor>()](WebView::DOMNodeProperties const& properties) {
                if (auto self = weak_self.strong_ref())
                    self->received_dom_node_properties(properties);
            });
    }
}

PageStyleActor::~PageStyleActor()
{
    if (auto tab = InspectorActor::tab_for(m_inspector))
        devtools().delegate().stop_listening_for_dom_properties(tab->description());
}

void PageStyleActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "getAllUsedFontFaces"_sv) {
        response.set("fontFaces"_sv, JsonArray {});
        send_response(message, move(response));
        return;
    }

    if (message.type == "getApplied"_sv) {
        // FIXME: This provides information to the "styles" pane in the inspector tab, which allows toggling and editing
        //        styles live. We do not yet support figuring out the list of styles that apply to a specific node.
        response.set("entries"_sv, JsonArray {});
        send_response(message, move(response));
        return;
    }

    if (message.type == "getComputed"_sv) {
        inspect_dom_node(message, WebView::DOMNodeProperties::Type::ComputedStyle);
        return;
    }

    if (message.type == "getLayout"_sv) {
        inspect_dom_node(message, WebView::DOMNodeProperties::Type::Layout);
        return;
    }

    if (message.type == "getUsedFontFaces"_sv) {
        inspect_dom_node(message, WebView::DOMNodeProperties::Type::UsedFonts);
        return;
    }

    if (message.type == "isPositionEditable") {
        response.set("value"_sv, false);
        send_response(message, move(response));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

JsonValue PageStyleActor::serialize_style() const
{
    JsonObject traits;
    traits.set("fontStyleLevel4"_sv, true);
    traits.set("fontWeightLevel4"_sv, true);
    traits.set("fontStretchLevel4"_sv, true);
    traits.set("fontVariations"_sv, true);

    JsonObject style;
    style.set("actor"_sv, name());
    style.set("traits"_sv, move(traits));
    return style;
}

void PageStyleActor::inspect_dom_node(Message const& message, WebView::DOMNodeProperties::Type property_type)
{
    auto node = get_required_parameter<String>(message, "node"_sv);
    if (!node.has_value())
        return;

    auto dom_node = WalkerActor::dom_node_for(InspectorActor::walker_for(m_inspector), *node);
    if (!dom_node.has_value()) {
        send_unknown_actor_error(message, *node);
        return;
    }

    devtools().delegate().inspect_dom_node(dom_node->tab->description(), property_type, dom_node->identifier.id, dom_node->identifier.pseudo_element);
    m_pending_inspect_requests.append({ .id = message.id });
}

void PageStyleActor::received_dom_node_properties(WebView::DOMNodeProperties const& properties)
{
    if (m_pending_inspect_requests.is_empty())
        return;

    JsonObject response;

    switch (properties.type) {
    case WebView::DOMNodeProperties::Type::ComputedStyle:
        if (properties.properties.is_object())
            received_computed_style(response, properties.properties.as_object());
        break;
    case WebView::DOMNodeProperties::Type::Layout:
        if (properties.properties.is_object())
            received_layout(response, properties.properties.as_object());
        break;
    case WebView::DOMNodeProperties::Type::UsedFonts:
        if (properties.properties.is_array())
            received_fonts(response, properties.properties.as_array());
        break;
    }

    auto message = m_pending_inspect_requests.take_first();
    send_response(message, move(response));
}

}
