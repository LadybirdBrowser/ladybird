/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/ScopeGuard.h>
#include <LibDevTools/Actors/InspectorActor.h>
#include <LibDevTools/Actors/PageStyleActor.h>
#include <LibDevTools/Actors/StyleRuleActor.h>
#include <LibDevTools/Actors/StyleSheetsActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/WalkerActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>

namespace DevTools {

static void received_layout(JsonObject& response, JsonValue const& node_box_sizing)
{
    if (!node_box_sizing.is_object())
        return;

    response.set("autoMargins"sv, JsonObject {});

    auto pixel_value = [&](auto key) {
        return node_box_sizing.as_object().get_double_with_precision_loss(key).value_or(0);
    };
    auto set_pixel_value = [&](auto key) {
        response.set(key, MUST(String::formatted("{}px", pixel_value(key))));
    };
    auto set_computed_value = [&](auto key) {
        response.set(key, node_box_sizing.as_object().get(key).value_or(String {}));
    };

    // FIXME: This response should also contain "top", "right", "bottom", and "left", but our box model metrics in
    //        WebContent do not provide this information.

    set_computed_value("width"sv);
    set_computed_value("height"sv);

    set_pixel_value("border-top-width"sv);
    set_pixel_value("border-right-width"sv);
    set_pixel_value("border-bottom-width"sv);
    set_pixel_value("border-left-width"sv);

    set_pixel_value("margin-top"sv);
    set_pixel_value("margin-right"sv);
    set_pixel_value("margin-bottom"sv);
    set_pixel_value("margin-left"sv);

    set_pixel_value("padding-top"sv);
    set_pixel_value("padding-right"sv);
    set_pixel_value("padding-bottom"sv);
    set_pixel_value("padding-left"sv);

    set_computed_value("box-sizing"sv);
    set_computed_value("display"sv);
    set_computed_value("float"sv);
    set_computed_value("line-height"sv);
    set_computed_value("position"sv);
    set_computed_value("z-index"sv);
}

static void received_computed_style(JsonObject& response, JsonValue const& computed_style)
{
    JsonObject computed;

    if (computed_style.is_object()) {
        computed_style.as_object().for_each_member([&](String const& name, JsonValue const& value) {
            JsonObject property;
            property.set("matched"sv, true);
            property.set("value"sv, value);
            computed.set(name, move(property));
        });
    }

    response.set("computed"sv, move(computed));
}

static void received_fonts(JsonObject& response, JsonValue const& fonts)
{
    JsonArray font_faces;

    if (fonts.is_array()) {
        fonts.as_array().for_each([&](JsonValue const& font) {
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
    }

    response.set("fontFaces"sv, move(font_faces));
}

static Optional<Web::CSS::StyleSheetIdentifier> style_sheet_identifier_from_json(JsonObject const& object)
{
    auto type_string = object.get_string("type"sv);
    if (!type_string.has_value())
        return {};

    auto type = Web::CSS::style_sheet_identifier_type_from_string(*type_string);
    if (!type.has_value())
        return {};

    auto dom_element_unique_id = object.get_integer<Web::UniqueNodeID::Type>("domElementUniqueId"sv);
    auto rule_count = object.get_integer<size_t>("ruleCount"sv).value_or(0);
    Optional<String> url;
    if (auto url_value = object.get_string("url"sv); url_value.has_value())
        url = *url_value;

    return Web::CSS::StyleSheetIdentifier {
        .type = *type,
        .dom_element_unique_id = dom_element_unique_id.map([](auto value) -> Web::UniqueNodeID { return value; }),
        .url = move(url),
        .rule_count = rule_count,
    };
}

static void link_rule_to_style_sheet_resource(WeakPtr<InspectorActor> const& inspector, JsonObject& rule)
{
    auto style_sheet = rule.get_object("styleSheet"sv);
    if (!style_sheet.has_value())
        return;

    ScopeGuard remove_internal_style_sheet_data = [&] {
        rule.remove("styleSheet"sv);
    };

    auto identifier = style_sheet_identifier_from_json(*style_sheet);
    if (!identifier.has_value())
        return;

    auto style_sheets_actor = InspectorActor::style_sheets_for(inspector);
    if (!style_sheets_actor)
        return;

    if (auto resource_id = style_sheets_actor->resource_id_for(*identifier); resource_id.has_value())
        rule.set("parentStyleSheet"sv, resource_id.release_value());
}

void PageStyleActor::received_applied_style_rules(JsonObject& response, JsonValue const& applied_style_rules)
{
    JsonArray entries;
    clear_style_rule_actors();

    if (applied_style_rules.is_array()) {
        applied_style_rules.as_array().for_each([&](JsonValue const& value) {
            if (!value.is_object())
                return;

            auto entry = value.as_object();
            auto rule = entry.get_object("rule"sv);
            if (!rule.has_value())
                return;

            link_rule_to_style_sheet_resource(m_inspector, *rule);

            auto& style_rule_actor = devtools().register_actor<StyleRuleActor>(*rule);
            m_style_rule_actors.append(style_rule_actor.name());
            entry.set("rule"sv, style_rule_actor.serialize_rule());

            if (auto inherited_node_id = entry.get_integer<Web::UniqueNodeID::Type>("inheritedNodeId"sv); inherited_node_id.has_value()) {
                JsonValue inherited { JsonValue {} };
                if (auto walker = InspectorActor::walker_for(m_inspector)) {
                    if (auto inherited_node_actor = walker->node_actor_name_for(Web::UniqueNodeID { *inherited_node_id }); inherited_node_actor.has_value())
                        inherited = *inherited_node_actor;
                }
                entry.set("inherited"sv, move(inherited));
                entry.remove("inheritedNodeId"sv);
            }

            entries.must_append(move(entry));
        });
    }

    response.set("entries"sv, move(entries));
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
            weak_callback(*this, [](auto& self, WebView::DOMNodeProperties const& properties) {
                self.received_dom_node_properties(properties);
            }));
    }
}

PageStyleActor::~PageStyleActor()
{
    clear_style_rule_actors();

    if (auto tab = InspectorActor::tab_for(m_inspector))
        devtools().delegate().stop_listening_for_dom_properties(tab->description());
}

void PageStyleActor::clear_style_rule_actors()
{
    for (auto const& actor : m_style_rule_actors)
        devtools().unregister_actor(actor);
    m_style_rule_actors.clear();
}

void PageStyleActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "getAllUsedFontFaces"sv) {
        response.set("fontFaces"sv, JsonArray {});
        send_response(message, move(response));
        return;
    }

    if (message.type == "getApplied"sv) {
        inspect_dom_node(message, WebView::DOMNodeProperties::Type::AppliedStyleRules);
        return;
    }

    if (message.type == "getComputed"sv) {
        inspect_dom_node(message, WebView::DOMNodeProperties::Type::ComputedStyle);
        return;
    }

    if (message.type == "getLayout"sv) {
        inspect_dom_node(message, WebView::DOMNodeProperties::Type::Layout);
        return;
    }

    if (message.type == "getUsedFontFaces"sv) {
        inspect_dom_node(message, WebView::DOMNodeProperties::Type::UsedFonts);
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

void PageStyleActor::inspect_dom_node(Message const& message, WebView::DOMNodeProperties::Type property_type)
{
    auto node = get_required_parameter<String>(message, "node"sv);
    if (!node.has_value())
        return;

    auto dom_node = WalkerActor::dom_node_for(InspectorActor::walker_for(m_inspector), *node);
    if (!dom_node.has_value()) {
        send_unknown_actor_error(message, *node);
        return;
    }

    JsonObject options;
    if (property_type == WebView::DOMNodeProperties::Type::AppliedStyleRules) {
        if (auto inherited = message.data.get_bool("inherited"sv); inherited.has_value())
            options.set("inherited"sv, *inherited);
        if (auto matched_selectors = message.data.get_bool("matchedSelectors"sv); matched_selectors.has_value())
            options.set("matchedSelectors"sv, *matched_selectors);
        if (auto skip_pseudo = message.data.get_bool("skipPseudo"sv); skip_pseudo.has_value())
            options.set("skipPseudo"sv, *skip_pseudo);
        if (auto filter = message.data.get_string("filter"sv); filter.has_value())
            options.set("filter"sv, *filter);
    }

    devtools().delegate().inspect_dom_node(dom_node->tab->description(), property_type, dom_node->identifier.id, dom_node->identifier.pseudo_element, move(options));
    m_pending_inspect_requests.append({ .id = message.id });
}

void PageStyleActor::received_dom_node_properties(WebView::DOMNodeProperties const& properties)
{
    if (m_pending_inspect_requests.is_empty())
        return;

    JsonObject response;

    switch (properties.type) {
    case WebView::DOMNodeProperties::Type::AppliedStyleRules:
        received_applied_style_rules(response, properties.properties);
        break;
    case WebView::DOMNodeProperties::Type::ComputedStyle:
        received_computed_style(response, properties.properties);
        break;
    case WebView::DOMNodeProperties::Type::Layout:
        received_layout(response, properties.properties);
        break;
    case WebView::DOMNodeProperties::Type::UsedFonts:
        received_fonts(response, properties.properties);
        break;
    }

    auto message = m_pending_inspect_requests.take_first();
    send_response(message, move(response));
}

}
