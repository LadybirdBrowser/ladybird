/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Enumerate.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/Resource.h>
#include <LibJS/MarkupGenerator.h>
#include <LibURL/Parser.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Namespace.h>
#include <LibWebView/Application.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/InspectorClient.h>
#include <LibWebView/SourceHighlighter.h>

namespace WebView {

static constexpr auto INSPECTOR_HTML = "resource://ladybird/inspector.html"sv;
static constexpr auto INSPECTOR_CSS = "resource://ladybird/inspector.css"sv;
static constexpr auto INSPECTOR_JS = "resource://ladybird/inspector.js"sv;

static String style_sheet_identifier_to_json(Web::CSS::StyleSheetIdentifier const& identifier)
{
    return MUST(String::formatted("{{ type: '{}', domNodeId: {}, url: '{}' }}"sv,
        Web::CSS::style_sheet_identifier_type_to_string(identifier.type),
        identifier.dom_element_unique_id.map([](auto& it) { return String::number(it.value()); }).value_or("undefined"_string),
        identifier.url.value_or("undefined"_string)));
}

InspectorClient::InspectorClient(ViewImplementation& content_web_view, ViewImplementation& inspector_web_view)
    : m_content_web_view(content_web_view)
    , m_inspector_web_view(inspector_web_view)
{
    m_content_web_view.on_received_dom_tree = [this](auto const& dom_tree) {
        auto dom_tree_html = generate_dom_tree(dom_tree);
        auto dom_tree_base64 = MUST(encode_base64(dom_tree_html.bytes()));

        auto script = MUST(String::formatted("inspector.loadDOMTree(\"{}\");", dom_tree_base64));
        m_inspector_web_view.run_javascript(script);

        m_dom_tree_loaded = true;

        if (m_pending_selection.has_value())
            select_node(m_pending_selection.release_value());
        else
            select_default_node();
    };

    m_content_web_view.on_received_dom_node_properties = [this](auto const& properties) {
        StringBuilder builder;

        // FIXME: Support box model metrics and ARIA properties.
        builder.append("inspector.createPropertyTables(\""sv);
        builder.append_escaped_for_json(properties.computed_style.serialized());
        builder.append("\", \""sv);
        builder.append_escaped_for_json(properties.resolved_style.serialized());
        builder.append("\", \""sv);
        builder.append_escaped_for_json(properties.custom_properties.serialized());
        builder.append("\");"sv);

        builder.append("inspector.createFontList(\""sv);
        builder.append_escaped_for_json(properties.fonts.serialized());
        builder.append("\");"sv);

        m_inspector_web_view.run_javascript(MUST(builder.to_string()));
    };

    m_content_web_view.on_received_accessibility_tree = [this](auto const& accessibility_tree) {
        auto accessibility_tree_html = generate_accessibility_tree(accessibility_tree);
        auto accessibility_tree_base64 = MUST(encode_base64(accessibility_tree_html.bytes()));

        auto script = MUST(String::formatted("inspector.loadAccessibilityTree(\"{}\");", accessibility_tree_base64));
        m_inspector_web_view.run_javascript(script);
    };

    m_content_web_view.on_received_hovered_node_id = [this](auto node_id) {
        select_node(node_id);
    };

    m_content_web_view.on_received_style_sheet_list = [this](auto const& style_sheets) {
        StringBuilder builder;
        builder.append("inspector.setStyleSheets(["sv);
        for (auto& style_sheet : style_sheets) {
            builder.appendff("{}, "sv, style_sheet_identifier_to_json(style_sheet));
        }
        builder.append("]);"sv);

        m_inspector_web_view.run_javascript(MUST(builder.to_string()));
    };

    m_content_web_view.on_received_style_sheet_source = [this](Web::CSS::StyleSheetIdentifier const& identifier, URL::URL const& base_url, String const& source) {
        auto html = highlight_source(URL::Parser::basic_parse(identifier.url.value_or({})), base_url, source, Syntax::Language::CSS, HighlightOutputMode::SourceOnly);
        auto script = MUST(String::formatted("inspector.setStyleSheetSource({}, \"{}\");",
            style_sheet_identifier_to_json(identifier),
            MUST(encode_base64(html.bytes()))));
        m_inspector_web_view.run_javascript(script);
    };

    m_content_web_view.on_finshed_editing_dom_node = [this](auto const& node_id) {
        m_pending_selection = node_id;
        m_dom_tree_loaded = false;
        m_dom_node_attributes.clear();

        inspect();
    };

    m_content_web_view.on_received_dom_node_html = [this](auto const& html) {
        if (m_content_web_view.on_insert_clipboard_entry)
            m_content_web_view.on_insert_clipboard_entry(html, "unspecified"_string, "text/plain"_string);
    };

    m_content_web_view.on_console_message_available = [this](auto message_index) {
        console_message_available(message_index);
    };

    m_content_web_view.on_received_styled_console_messages = [this](auto start_index, auto const& message_types, auto const& messages) {
        console_messages_received(start_index, message_types, messages);
    };

    m_inspector_web_view.enable_inspector_prototype();
    m_inspector_web_view.use_native_user_style_sheet();

    m_inspector_web_view.on_inspector_loaded = [this]() {
        m_inspector_loaded = true;
        inspect();

        m_content_web_view.js_console_request_messages(0);
    };

    m_inspector_web_view.on_inspector_requested_dom_tree_context_menu = [this](auto node_id, auto position, auto const& type, auto const& tag, auto const& attribute_index) {
        Optional<Attribute> attribute;
        if (attribute_index.has_value())
            attribute = m_dom_node_attributes.get(node_id)->at(*attribute_index);

        m_context_menu_data = ContextMenuData { node_id, tag, attribute };

        if (type.is_one_of("text"sv, "comment"sv)) {
            if (on_requested_dom_node_text_context_menu)
                on_requested_dom_node_text_context_menu(position);
        } else if (type == "tag"sv) {
            VERIFY(tag.has_value());

            if (on_requested_dom_node_tag_context_menu)
                on_requested_dom_node_tag_context_menu(position, *tag);
        } else if (type == "attribute"sv) {
            VERIFY(tag.has_value());
            VERIFY(attribute.has_value());

            if (on_requested_dom_node_attribute_context_menu)
                on_requested_dom_node_attribute_context_menu(position, *tag, *attribute);
        }
    };

    m_inspector_web_view.on_inspector_selected_dom_node = [this](auto node_id, auto const& pseudo_element) {
        m_content_web_view.highlight_dom_node(node_id, pseudo_element);
        m_content_web_view.inspect_dom_node(node_id, pseudo_element);
    };

    m_inspector_web_view.on_inspector_set_dom_node_text = [this](auto node_id, auto const& text) {
        m_content_web_view.set_dom_node_text(node_id, text);
    };

    m_inspector_web_view.on_inspector_set_dom_node_tag = [this](auto node_id, auto const& tag) {
        m_content_web_view.set_dom_node_tag(node_id, tag);
    };

    m_inspector_web_view.on_inspector_added_dom_node_attributes = [this](auto node_id, auto const& attributes) {
        m_content_web_view.add_dom_node_attributes(node_id, attributes);
    };

    m_inspector_web_view.on_inspector_replaced_dom_node_attribute = [this](auto node_id, u32 attribute_index, auto const& replacement_attributes) {
        auto const& attribute = m_dom_node_attributes.get(node_id)->at(attribute_index);
        m_content_web_view.replace_dom_node_attribute(node_id, attribute.name, replacement_attributes);
    };

    m_inspector_web_view.on_inspector_requested_cookie_context_menu = [this](auto cookie_index, auto position) {
        if (cookie_index >= m_cookies.size())
            return;

        m_cookie_context_menu_index = cookie_index;

        if (on_requested_cookie_context_menu)
            on_requested_cookie_context_menu(position, m_cookies[cookie_index]);
    };

    m_inspector_web_view.on_inspector_requested_style_sheet_source = [this](auto const& identifier) {
        m_content_web_view.request_style_sheet_source(identifier);
    };

    m_inspector_web_view.on_inspector_executed_console_script = [this](auto const& script) {
        append_console_source(script);

        m_content_web_view.js_console_input(script);
    };

    m_inspector_web_view.on_inspector_exported_inspector_html = [this](String const& html) {
        auto maybe_inspector_path = Application::the().path_for_downloaded_file("inspector"sv);

        if (maybe_inspector_path.is_error()) {
            append_console_warning(MUST(String::formatted("Unable to select a download location: {}", maybe_inspector_path.error())));
            return;
        }

        auto inspector_path = maybe_inspector_path.release_value();

        if (auto result = Core::Directory::create(inspector_path.string(), Core::Directory::CreateDirectories::Yes); result.is_error()) {
            append_console_warning(MUST(String::formatted("Unable to create {}: {}", inspector_path, result.error())));
            return;
        }

        auto export_file = [&](auto name, auto const& contents) {
            auto path = inspector_path.append(name);

            auto file = Core::File::open(path.string(), Core::File::OpenMode::Write);
            if (file.is_error()) {
                append_console_warning(MUST(String::formatted("Unable to open {}: {}", path, file.error())));
                return false;
            }

            if (auto result = file.value()->write_until_depleted(contents); result.is_error()) {
                append_console_warning(MUST(String::formatted("Unable to save {}: {}", path, result.error())));
                return false;
            }

            return true;
        };

        auto inspector_css = MUST(Core::Resource::load_from_uri(INSPECTOR_CSS));
        auto inspector_js = MUST(Core::Resource::load_from_uri(INSPECTOR_JS));

        auto inspector_html = MUST(html.replace(INSPECTOR_CSS, "inspector.css"sv, ReplaceMode::All));
        inspector_html = MUST(inspector_html.replace(INSPECTOR_JS, "inspector.js"sv, ReplaceMode::All));

        if (!export_file("inspector.html"sv, inspector_html))
            return;
        if (!export_file("inspector.css"sv, inspector_css->data()))
            return;
        if (!export_file("inspector.js"sv, inspector_js->data()))
            return;

        append_console_message(MUST(String::formatted("Exported Inspector files to {}", inspector_path)));
    };

    load_inspector();
}

InspectorClient::~InspectorClient()
{
    m_content_web_view.on_finshed_editing_dom_node = nullptr;
    m_content_web_view.on_received_accessibility_tree = nullptr;
    m_content_web_view.on_console_message_available = nullptr;
    m_content_web_view.on_received_styled_console_messages = nullptr;
    m_content_web_view.on_received_dom_node_html = nullptr;
    m_content_web_view.on_received_dom_node_properties = nullptr;
    m_content_web_view.on_received_dom_tree = nullptr;
    m_content_web_view.on_received_hovered_node_id = nullptr;
    m_content_web_view.on_received_style_sheet_list = nullptr;
    m_content_web_view.on_inspector_requested_style_sheet_source = nullptr;
}

void InspectorClient::inspect()
{
    if (!m_inspector_loaded)
        return;

    m_content_web_view.inspect_dom_tree();
    m_content_web_view.inspect_accessibility_tree();
    m_content_web_view.list_style_sheets();
    load_cookies();
}

void InspectorClient::reset()
{
    static auto script = "inspector.reset();"_string;
    m_inspector_web_view.run_javascript(script);

    m_body_or_frameset_node_id.clear();
    m_pending_selection.clear();
    m_dom_tree_loaded = false;

    m_dom_node_attributes.clear();

    m_highest_notified_message_index = -1;
    m_highest_received_message_index = -1;
    m_waiting_for_messages = false;
}

void InspectorClient::select_hovered_node()
{
    m_content_web_view.get_hovered_node_id();
}

void InspectorClient::select_default_node()
{
    if (m_body_or_frameset_node_id.has_value())
        select_node(*m_body_or_frameset_node_id);
}

void InspectorClient::clear_selection()
{
    m_content_web_view.clear_highlighted_dom_node();
    m_content_web_view.clear_inspected_dom_node();

    static auto script = "inspector.clearInspectedDOMNode();"_string;
    m_inspector_web_view.run_javascript(script);
}

void InspectorClient::select_node(Web::UniqueNodeID node_id)
{
    if (!m_dom_tree_loaded) {
        m_pending_selection = node_id;
        return;
    }

    auto script = MUST(String::formatted("inspector.inspectDOMNodeID({});", node_id.value()));
    m_inspector_web_view.run_javascript(script);
}

void InspectorClient::load_cookies()
{
    m_cookies = Application::cookie_jar().get_all_cookies(m_content_web_view.url());
    JsonArray json_cookies;

    for (auto const& [index, cookie] : enumerate(m_cookies)) {
        JsonObject json_cookie;

        json_cookie.set("index"sv, JsonValue { index });
        json_cookie.set("name"sv, JsonValue { cookie.name });
        json_cookie.set("value"sv, JsonValue { cookie.value });
        json_cookie.set("domain"sv, JsonValue { cookie.domain });
        json_cookie.set("path"sv, JsonValue { cookie.path });
        json_cookie.set("creationTime"sv, JsonValue { cookie.creation_time.milliseconds_since_epoch() });
        json_cookie.set("lastAccessTime"sv, JsonValue { cookie.last_access_time.milliseconds_since_epoch() });
        json_cookie.set("expiryTime"sv, JsonValue { cookie.expiry_time.milliseconds_since_epoch() });

        MUST(json_cookies.append(move(json_cookie)));
    }

    StringBuilder builder;
    builder.append("inspector.setCookies("sv);
    json_cookies.serialize(builder);
    builder.append(");"sv);

    m_inspector_web_view.run_javascript(MUST(builder.to_string()));
}

void InspectorClient::context_menu_edit_dom_node()
{
    VERIFY(m_context_menu_data.has_value());

    auto script = MUST(String::formatted("inspector.editDOMNodeID({});", m_context_menu_data->dom_node_id));
    m_inspector_web_view.run_javascript(script);

    m_context_menu_data.clear();
}

void InspectorClient::context_menu_copy_dom_node()
{
    VERIFY(m_context_menu_data.has_value());

    m_content_web_view.get_dom_node_html(m_context_menu_data->dom_node_id);
    m_context_menu_data.clear();
}

void InspectorClient::context_menu_screenshot_dom_node()
{
    VERIFY(m_context_menu_data.has_value());

    m_content_web_view.take_dom_node_screenshot(m_context_menu_data->dom_node_id)
        ->when_resolved([this](auto const& path) {
            append_console_message(MUST(String::formatted("Screenshot saved to: {}", path)));
        })
        .when_rejected([this](auto const& error) {
            append_console_warning(MUST(String::formatted("Warning: {}", error)));
        });

    m_context_menu_data.clear();
}

void InspectorClient::context_menu_create_child_element()
{
    VERIFY(m_context_menu_data.has_value());

    m_content_web_view.create_child_element(m_context_menu_data->dom_node_id);
    m_context_menu_data.clear();
}

void InspectorClient::context_menu_create_child_text_node()
{
    VERIFY(m_context_menu_data.has_value());

    m_content_web_view.create_child_text_node(m_context_menu_data->dom_node_id);
    m_context_menu_data.clear();
}

void InspectorClient::context_menu_clone_dom_node()
{
    VERIFY(m_context_menu_data.has_value());

    m_content_web_view.clone_dom_node(m_context_menu_data->dom_node_id);
    m_context_menu_data.clear();
}

void InspectorClient::context_menu_remove_dom_node()
{
    VERIFY(m_context_menu_data.has_value());

    m_content_web_view.remove_dom_node(m_context_menu_data->dom_node_id);
    m_context_menu_data.clear();
}

void InspectorClient::context_menu_add_dom_node_attribute()
{
    VERIFY(m_context_menu_data.has_value());

    auto script = MUST(String::formatted("inspector.addAttributeToDOMNodeID({});", m_context_menu_data->dom_node_id));
    m_inspector_web_view.run_javascript(script);

    m_context_menu_data.clear();
}

void InspectorClient::context_menu_remove_dom_node_attribute()
{
    VERIFY(m_context_menu_data.has_value());
    VERIFY(m_context_menu_data->attribute.has_value());

    m_content_web_view.replace_dom_node_attribute(m_context_menu_data->dom_node_id, m_context_menu_data->attribute->name, {});
    m_context_menu_data.clear();
}

void InspectorClient::context_menu_copy_dom_node_attribute_value()
{
    VERIFY(m_context_menu_data.has_value());
    VERIFY(m_context_menu_data->attribute.has_value());

    if (m_content_web_view.on_insert_clipboard_entry)
        m_content_web_view.on_insert_clipboard_entry(m_context_menu_data->attribute->value, "unspecified"_string, "text/plain"_string);

    m_context_menu_data.clear();
}

void InspectorClient::context_menu_delete_cookie()
{
    VERIFY(m_cookie_context_menu_index.has_value());
    VERIFY(*m_cookie_context_menu_index < m_cookies.size());

    auto& cookie = m_cookies[*m_cookie_context_menu_index];
    cookie.expiry_time = UnixDateTime::earliest();

    Application::cookie_jar().update_cookie(move(cookie));
    load_cookies();

    m_cookie_context_menu_index.clear();
}

void InspectorClient::context_menu_delete_all_cookies()
{
    for (auto& cookie : m_cookies) {
        cookie.expiry_time = UnixDateTime::earliest();

        Application::cookie_jar().update_cookie(move(cookie));
    }

    load_cookies();

    m_cookie_context_menu_index.clear();
}

void InspectorClient::load_inspector()
{
    auto inspector_html = MUST(Core::Resource::load_from_uri(INSPECTOR_HTML));

    auto generate_property_table = [&](auto name) {
        return MUST(String::formatted(R"~~~(
            <div id="{0}" class="tab-content">
                <input class="property-filter" id="{0}-filter" placeholder="Filter properties" />
                <table class="property-table">
                    <thead>
                        <tr>
                            <th>Name</th>
                            <th>Value</th>
                        </tr>
                    </thead>
                    <tbody id="{0}-table">
                    </tbody>
                </table>
            </div>
)~~~",
            name));
    };

    StringBuilder builder;

    SourceGenerator generator { builder };
    generator.set("INSPECTOR_CSS"sv, INSPECTOR_CSS);
    generator.set("INSPECTOR_JS"sv, INSPECTOR_JS);
    generator.set("INSPECTOR_STYLE"sv, HTML_HIGHLIGHTER_STYLE);
    generator.set("COMPUTED_STYLE"sv, generate_property_table("computed-style"sv));
    generator.set("RESOVLED_STYLE"sv, generate_property_table("resolved-style"sv));
    generator.set("CUSTOM_PROPERTIES"sv, generate_property_table("custom-properties"sv));
    generator.append(inspector_html->data());

    m_inspector_web_view.load_html(generator.as_string_view());
}

template<typename Generator>
static void generate_tree(StringBuilder& builder, JsonObject const& node, Generator&& generator)
{
    if (auto children = node.get_array("children"sv); children.has_value() && !children->is_empty()) {
        auto name = node.get_string("name"sv).value_or({});
        builder.append("<details>"sv);

        builder.append("<summary>"sv);
        generator(node);
        builder.append("</summary>"sv);

        children->for_each([&](auto const& child) {
            builder.append("<div>"sv);
            generate_tree(builder, child.as_object(), generator);
            builder.append("</div>"sv);
        });

        builder.append("</details>"sv);
    } else {
        generator(node);
    }
}

String InspectorClient::generate_dom_tree(JsonObject const& dom_tree)
{
    StringBuilder builder;

    generate_tree(builder, dom_tree, [&](JsonObject const& node) {
        auto type = node.get_string("type"sv).value_or("unknown"_string);
        auto name = node.get_string("name"sv).value_or({});

        StringBuilder data_attributes;
        auto append_data_attribute = [&](auto name, auto value) {
            if (!data_attributes.is_empty())
                data_attributes.append(' ');
            data_attributes.appendff("data-{}=\"{}\"", name, value);
        };

        i32 node_id = 0;

        if (auto pseudo_element = node.get_integer<i32>("pseudo-element"sv); pseudo_element.has_value()) {
            node_id = node.get_integer<i32>("parent-id"sv).value();
            append_data_attribute("pseudo-element"sv, *pseudo_element);
        } else {
            node_id = node.get_integer<i32>("id"sv).value();
        }

        append_data_attribute("id"sv, node_id);

        if (type == "text"sv) {
            auto deprecated_text = escape_html_entities(*node.get_string("text"sv));
            auto text = MUST(Web::Infra::strip_and_collapse_whitespace(deprecated_text));

            builder.appendff("<span data-node-type=\"text\" class=\"hoverable editable\" {}>", data_attributes.string_view());

            if (text.is_empty())
                builder.appendff("<span class=\"internal\">{}</span>", name);
            else
                builder.append(text);

            builder.append("</span>"sv);
            return;
        }

        if (type == "comment"sv) {
            auto comment = escape_html_entities(*node.get_string("data"sv));

            builder.appendff("<span class=\"hoverable comment\" {}>", data_attributes.string_view());
            builder.append("<span>&lt;!--</span>"sv);
            builder.appendff("<span data-node-type=\"comment\" class=\"editable\">{}</span>", comment);
            builder.append("<span>--&gt;</span>"sv);
            builder.append("</span>"sv);
            return;
        }

        if (type == "shadow-root"sv) {
            auto mode = node.get_string("mode"sv).release_value();

            builder.appendff("<span class=\"hoverable internal\" {}>", data_attributes.string_view());
            builder.appendff("{} ({})", name, mode);
            builder.append("</span>"sv);
            return;
        }

        if (type != "element"sv) {
            builder.appendff("<span class=\"hoverable internal\" {}>", data_attributes.string_view());
            builder.appendff(name);
        } else {
            if (name.equals_ignoring_ascii_case("BODY"sv) || name.equals_ignoring_ascii_case("FRAMESET"sv))
                m_body_or_frameset_node_id = node_id;

            auto tag = name;
            if (node.get_string("namespace"sv) == Web::Namespace::HTML.bytes_as_string_view())
                tag = MUST(tag.to_lowercase());

            builder.appendff("<span class=\"hoverable\" {}>", data_attributes.string_view());
            builder.append("<span>&lt;</span>"sv);
            builder.appendff("<span data-node-type=\"tag\" data-tag=\"{0}\" class=\"editable tag\">{0}</span>", tag);

            if (auto attributes = node.get_object("attributes"sv); attributes.has_value()) {
                attributes->for_each_member([&](auto const& name, auto const& value) {
                    auto& dom_node_attributes = m_dom_node_attributes.ensure(node_id);
                    auto value_string = value.as_string();

                    builder.append("&nbsp;"sv);
                    builder.appendff("<span data-node-type=\"attribute\" data-tag=\"{}\" data-attribute-index={} class=\"editable\">", tag, dom_node_attributes.size());
                    builder.appendff("<span class=\"attribute-name\">{}</span>", escape_html_entities(name));
                    builder.append('=');
                    builder.appendff("<span class=\"attribute-value\">\"{}\"</span>", escape_html_entities(value_string));
                    builder.append("</span>"sv);

                    dom_node_attributes.empend(name, value_string);
                });
            }

            builder.append("<span>&gt;</span>"sv);
        }

        // display miscellaneous extra bits of info about the element
        Vector<String> extra;
        if (node.get_bool("scrollable"sv).value_or(false)) {
            extra.append("scrollable"_string);
        }
        if (node.get_bool("invisible"sv).value_or(false)) {
            extra.append("invisible"_string);
        }
        if (node.get_bool("stackingContext"sv).value_or(false)) {
            extra.append("isolated"_string);
        }
        if (!extra.is_empty()) {
            builder.append(" <span>("sv);
            builder.append(extra[0]);
            for (size_t i = 1; i < extra.size(); i++) {
                builder.appendff(", {}", extra[i]);
            }
            builder.append(")</span>"sv);
        }

        builder.append("</span>"sv);
    });

    return MUST(builder.to_string());
}

String InspectorClient::generate_accessibility_tree(JsonObject const& accessibility_tree)
{
    StringBuilder builder;

    generate_tree(builder, accessibility_tree, [&](JsonObject const& node) {
        auto type = node.get_string("type"sv).value_or("unknown"_string);
        auto role = node.get_string("role"sv).value_or({});

        if (type == "text"sv) {
            auto text = escape_html_entities(*node.get_string("text"sv));

            builder.appendff("<span class=\"hoverable\">");
            builder.append(MUST(Web::Infra::strip_and_collapse_whitespace(text)));
            builder.append("</span>"sv);
            return;
        }

        if (type != "element"sv) {
            builder.appendff("<span class=\"hoverable internal\">");
            builder.appendff(MUST(role.to_lowercase()));
            builder.append("</span>"sv);
            return;
        }

        auto name = node.get_string("name"sv).value_or({});
        auto description = node.get_string("description"sv).value_or({});

        builder.appendff("<span class=\"hoverable\">");
        builder.append(MUST(role.to_lowercase()));
        builder.appendff(" name: \"{}\", description: \"{}\"", name, description);
        builder.append("</span>"sv);
    });

    return MUST(builder.to_string());
}

void InspectorClient::request_console_messages()
{
    VERIFY(!m_waiting_for_messages);

    m_content_web_view.js_console_request_messages(m_highest_received_message_index + 1);
    m_waiting_for_messages = true;
}

void InspectorClient::console_message_available(i32 message_index)
{
    if (message_index <= m_highest_received_message_index) {
        dbgln("Notified about console message we already have");
        return;
    }
    if (message_index <= m_highest_notified_message_index) {
        dbgln("Notified about console message we're already aware of");
        return;
    }

    m_highest_notified_message_index = message_index;

    if (!m_waiting_for_messages)
        request_console_messages();
}

void InspectorClient::console_messages_received(i32 start_index, ReadonlySpan<String> message_types, ReadonlySpan<String> messages)
{
    auto end_index = start_index + static_cast<i32>(message_types.size()) - 1;
    if (end_index <= m_highest_received_message_index) {
        dbgln("Received old console messages");
        return;
    }

    for (size_t i = 0; i < message_types.size(); ++i) {
        auto const& type = message_types[i];
        auto const& message = messages[i];

        if (type == "html"sv)
            append_console_output(message);
        else if (type == "clear"sv)
            clear_console_output();
        else if (type == "group"sv)
            begin_console_group(message, true);
        else if (type == "groupCollapsed"sv)
            begin_console_group(message, false);
        else if (type == "groupEnd"sv)
            end_console_group();
        else
            VERIFY_NOT_REACHED();
    }

    m_highest_received_message_index = end_index;
    m_waiting_for_messages = false;

    if (m_highest_received_message_index < m_highest_notified_message_index)
        request_console_messages();
}

void InspectorClient::append_console_source(StringView source)
{
    StringBuilder builder;
    builder.append("<span class=\"console-prompt\">&gt;&nbsp;</span>"sv);
    builder.append(MUST(JS::MarkupGenerator::html_from_source(source)));

    append_console_output(builder.string_view());
}

void InspectorClient::append_console_message(StringView message)
{
    StringBuilder builder;
    builder.append("<span class=\"console-prompt\">#&nbsp;</span>"sv);
    builder.appendff("<span class=\"console-message\">{}</span>", message);

    append_console_output(builder.string_view());
}

void InspectorClient::append_console_warning(StringView warning)
{
    StringBuilder builder;
    builder.append("<span class=\"console-prompt\">#&nbsp;</span>"sv);
    builder.appendff("<span class=\"console-warning\">{}</span>", warning);

    append_console_output(builder.string_view());
}

void InspectorClient::append_console_output(StringView html)
{
    auto html_base64 = MUST(encode_base64(html.bytes()));

    auto script = MUST(String::formatted("inspector.appendConsoleOutput(\"{}\");", html_base64));
    m_inspector_web_view.run_javascript(script);
}

void InspectorClient::clear_console_output()
{
    static auto script = "inspector.clearConsoleOutput();"_string;
    m_inspector_web_view.run_javascript(script);
}

void InspectorClient::begin_console_group(StringView label, bool start_expanded)
{
    auto label_base64 = MUST(encode_base64(label.bytes()));

    auto script = MUST(String::formatted("inspector.beginConsoleGroup(\"{}\", {});", label_base64, start_expanded));
    m_inspector_web_view.run_javascript(script);
}

void InspectorClient::end_console_group()
{
    static auto script = "inspector.endConsoleGroup();"_string;
    m_inspector_web_view.run_javascript(script);
}

}
