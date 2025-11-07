/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2023-2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/QuickSort.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibGC/Heap.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/SystemTheme.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/Date.h>
#include <LibUnicode/TimeZone.h>
#include <LibWeb/ARIA/RoleType.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/Parser/ErrorReporter.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleSheetList.h>
#include <LibWeb/CookieStore/CookieStore.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWeb/HTML/Storage.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Loader/ContentFilter.h>
#include <LibWeb/Loader/ProxyMappings.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Loader/UserAgent.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/PermissionsPolicy/AutoplayAllowlist.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWebView/Attribute.h>
#include <LibWebView/ViewImplementation.h>
#include <WebContent/ConnectionFromClient.h>
#include <WebContent/PageClient.h>
#include <WebContent/PageHost.h>
#include <WebContent/WebContentClientEndpoint.h>

namespace WebContent {

ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<WebContentClientEndpoint, WebContentServerEndpoint>(*this, move(transport), 1)
    , m_page_host(PageHost::create(*this))
{
}

ConnectionFromClient::~ConnectionFromClient() = default;

void ConnectionFromClient::die()
{
    Web::Platform::EventLoopPlugin::the().quit();
}

Messages::WebContentServer::InitTransportResponse ConnectionFromClient::init_transport([[maybe_unused]] int peer_pid)
{
#ifdef AK_OS_WINDOWS
    m_transport->set_peer_pid(peer_pid);
    return Core::System::getpid();
#endif
    VERIFY_NOT_REACHED();
}

Optional<PageClient&> ConnectionFromClient::page(u64 index, SourceLocation location)
{
    if (auto page = m_page_host->page(index); page.has_value())
        return *page;

    dbgln("ConnectionFromClient::{}: Did not find a page with ID {}", location.function_name(), index);
    return {};
}

Optional<PageClient const&> ConnectionFromClient::page(u64 index, SourceLocation location) const
{
    if (auto page = m_page_host->page(index); page.has_value())
        return *page;

    dbgln("ConnectionFromClient::{}: Did not find a page with ID {}", location.function_name(), index);
    return {};
}

void ConnectionFromClient::close_server()
{
    shutdown();
}

Messages::WebContentServer::GetWindowHandleResponse ConnectionFromClient::get_window_handle(u64 page_id)
{
    if (auto page = this->page(page_id); page.has_value())
        return page->page().top_level_traversable()->window_handle();
    return String {};
}

void ConnectionFromClient::set_window_handle(u64 page_id, String handle)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().top_level_traversable()->set_window_handle(move(handle));
}

void ConnectionFromClient::connect_to_webdriver(u64 page_id, ByteString webdriver_ipc_path)
{
    if (auto page = this->page(page_id); page.has_value()) {
        // FIXME: Propagate this error back to the browser.
        if (auto result = page->connect_to_webdriver(webdriver_ipc_path); result.is_error())
            dbgln("Unable to connect to the WebDriver process: {}", result.error());
    }
}

void ConnectionFromClient::connect_to_web_ui(u64 page_id, IPC::File web_ui_socket)
{
    if (auto page = this->page(page_id); page.has_value()) {
        // FIXME: Propagate this error back to the browser.
        if (auto result = page->connect_to_web_ui(move(web_ui_socket)); result.is_error())
            dbgln("Unable to connect to the WebUI host: {}", result.error());
    }
}

void ConnectionFromClient::connect_to_image_decoder(IPC::File image_decoder_socket)
{
    if (on_image_decoder_connection)
        on_image_decoder_connection(image_decoder_socket);
}

void ConnectionFromClient::connect_to_request_server(IPC::File request_server_socket)
{
    if (on_request_server_connection)
        on_request_server_connection(request_server_socket);
}

void ConnectionFromClient::update_system_theme(u64 page_id, Core::AnonymousBuffer theme_buffer)
{
    auto page = this->page(page_id);
    if (!page.has_value())
        return;

    Gfx::set_system_theme(theme_buffer);
    auto impl = Gfx::PaletteImpl::create_with_anonymous_buffer(theme_buffer);
    page->set_palette_impl(*impl);
}

void ConnectionFromClient::update_screen_rects(u64 page_id, Vector<Web::DevicePixelRect> rects, u32 main_screen)
{
    if (auto page = this->page(page_id); page.has_value())
        page->set_screen_rects(rects, main_screen);
}

void ConnectionFromClient::load_url(u64 page_id, URL::URL url)
{
    auto page = this->page(page_id);
    if (!page.has_value())
        return;

    page->page().load(url);
}

void ConnectionFromClient::load_html(u64 page_id, ByteString html)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().load_html(html);
}

void ConnectionFromClient::reload(u64 page_id)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().reload();
}

void ConnectionFromClient::traverse_the_history_by_delta(u64 page_id, i32 delta)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().traverse_the_history_by_delta(delta);
}

void ConnectionFromClient::set_viewport_size(u64 page_id, Web::DevicePixelSize size)
{
    if (auto page = this->page(page_id); page.has_value())
        page->set_viewport_size(size);
}

void ConnectionFromClient::ready_to_paint(u64 page_id)
{
    if (auto page = this->page(page_id); page.has_value())
        page->ready_to_paint();
}

void ConnectionFromClient::key_event(u64 page_id, Web::KeyEvent event)
{
    enqueue_input_event({ page_id, move(event), 0 });
}

void ConnectionFromClient::mouse_event(u64 page_id, Web::MouseEvent event)
{
    // OPTIMIZATION: Coalesce consecutive unprocessed mouse move and wheel events.
    auto event_to_coalesce = [&]() -> Web::MouseEvent const* {
        if (m_input_event_queue.is_empty())
            return nullptr;
        if (m_input_event_queue.tail().page_id != page_id)
            return nullptr;

        if (event.type != Web::MouseEvent::Type::MouseMove && event.type != Web::MouseEvent::Type::MouseWheel)
            return nullptr;

        if (auto const* mouse_event = m_input_event_queue.tail().event.get_pointer<Web::MouseEvent>()) {
            if (mouse_event->type == event.type)
                return mouse_event;
        }

        return nullptr;
    };

    if (auto const* last_mouse_event = event_to_coalesce()) {
        event.wheel_delta_x += last_mouse_event->wheel_delta_x;
        event.wheel_delta_y += last_mouse_event->wheel_delta_y;

        m_input_event_queue.tail().event = move(event);
        ++m_input_event_queue.tail().coalesced_event_count;

        return;
    }

    enqueue_input_event({ page_id, move(event), 0 });
}

void ConnectionFromClient::drag_event(u64 page_id, Web::DragEvent event)
{
    enqueue_input_event({ page_id, move(event), 0 });
}

void ConnectionFromClient::pinch_event(u64 page_id, Web::PinchEvent event)
{
    enqueue_input_event({ page_id, move(event), 0 });
}

void ConnectionFromClient::enqueue_input_event(Web::QueuedInputEvent event)
{
    m_input_event_queue.enqueue(move(event));
}

void ConnectionFromClient::debug_request(u64 page_id, ByteString request, ByteString argument)
{
    auto page = this->page(page_id);
    if (!page.has_value())
        return;

    if (request == "dump-session-history") {
        auto const& traversable = page->page().top_level_traversable();
        Web::dump_tree(*traversable);
        return;
    }

    if (request == "dump-display-list") {
        if (auto* doc = page->page().top_level_browsing_context().active_document()) {
            auto display_list_dump = doc->dump_display_list();
            dbgln("{}", display_list_dump);
        }
        return;
    }

    if (request == "dump-dom-tree") {
        if (auto* doc = page->page().top_level_browsing_context().active_document())
            Web::dump_tree(*doc);
        return;
    }

    if (request == "dump-layout-tree") {
        if (auto* doc = page->page().top_level_browsing_context().active_document()) {
            if (auto* viewport = doc->layout_node())
                Web::dump_tree(*viewport);
        }
        return;
    }

    if (request == "dump-paint-tree") {
        if (auto* doc = page->page().top_level_browsing_context().active_document()) {
            if (auto* paintable = doc->paintable())
                Web::dump_tree(*paintable);
        }
        return;
    }

    if (request == "dump-stacking-context-tree") {
        if (auto* doc = page->page().top_level_browsing_context().active_document()) {
            if (auto* viewport = doc->layout_node()) {
                auto& viewport_paintable = static_cast<Web::Painting::ViewportPaintable&>(*viewport->paintable_box());
                viewport_paintable.build_stacking_context_tree_if_needed();
                if (auto* stacking_context = viewport_paintable.stacking_context()) {
                    StringBuilder builder;
                    stacking_context->dump(builder);
                    dbgln("{}", builder.string_view());
                }
            }
        }
        return;
    }

    if (request == "dump-style-sheets") {
        if (auto* doc = page->page().top_level_browsing_context().active_document()) {
            dbgln("=== In document: ===");
            for (auto& sheet : doc->style_sheets().sheets()) {
                Web::dump_sheet(sheet);
            }

            doc->for_each_shadow_root([&](auto& shadow_root) {
                dbgln("=== In shadow root {}: ===", shadow_root.host()->debug_description());
                shadow_root.for_each_css_style_sheet([&](auto& sheet) {
                    Web::dump_sheet(sheet);
                });
            });
        }
        return;
    }

    if (request == "dump-all-resolved-styles") {
        auto dump_style = [](String const& title, Web::CSS::ComputedProperties const& style, RefPtr<Web::CSS::CustomPropertyData const> custom_property_data) {
            dbgln("+ {}", title);
            for (size_t i = to_underlying(Web::CSS::first_longhand_property_id); i < to_underlying(Web::CSS::last_longhand_property_id); ++i) {
                dbgln("|  {} = {}", Web::CSS::string_from_property_id(static_cast<Web::CSS::PropertyID>(i)), style.property(static_cast<Web::CSS::PropertyID>(i)).to_string(Web::CSS::SerializationMode::Normal));
            }
            if (custom_property_data) {
                custom_property_data->for_each_property([](FlyString const& name, Web::CSS::StyleProperty const& property) {
                    dbgln("|  {} = {}", name, property.value->to_string(Web::CSS::SerializationMode::Normal));
                });
            }
            dbgln("---");
        };

        if (auto* doc = page->page().top_level_browsing_context().active_document()) {
            Queue<Web::DOM::Node*> nodes_to_visit;
            nodes_to_visit.enqueue(doc->document_element());
            while (!nodes_to_visit.is_empty()) {
                auto node = nodes_to_visit.dequeue();
                for (auto& child : node->children_as_vector())
                    nodes_to_visit.enqueue(child.ptr());
                if (auto* element = as_if<Web::DOM::Element>(node)) {
                    auto styles = doc->style_computer().compute_style({ *element });
                    dump_style(MUST(String::formatted("Element {}", node->debug_description())), styles, element->custom_property_data({}));

                    for (auto pseudo_element_index = 0; pseudo_element_index < to_underlying(Web::CSS::PseudoElement::KnownPseudoElementCount); ++pseudo_element_index) {
                        auto pseudo_element_type = static_cast<Web::CSS::PseudoElement>(pseudo_element_index);
                        if (auto pseudo_element = element->get_pseudo_element(pseudo_element_type); pseudo_element.has_value() && pseudo_element->computed_properties()) {
                            dump_style(MUST(String::formatted("PseudoElement {}::{}", node->debug_description(), Web::CSS::pseudo_element_name(pseudo_element_type))), *pseudo_element->computed_properties(), pseudo_element->custom_property_data());
                        }
                    }
                }
            }
        }
        return;
    }

    if (request == "dump-all-css-errors") {
        Web::CSS::Parser::ErrorReporter::the().dump();
        return;
    }

    if (request == "collect-garbage") {
        // NOTE: We use deferred_invoke here to ensure that GC runs with as little on the stack as possible.
        Core::deferred_invoke([] {
            Web::Bindings::main_thread_vm().heap().collect_garbage(GC::Heap::CollectionType::CollectGarbage, true);
        });
        return;
    }

    if (request == "set-line-box-borders") {
        bool state = argument == "on";
        auto traversable = page->page().top_level_traversable();
        traversable->set_should_show_line_box_borders(state);
        traversable->set_needs_repaint();
        return;
    }

    if (request == "clear-cache") {
        Web::Fetch::Fetching::clear_http_memory_cache();
        return;
    }

    if (request == "spoof-user-agent") {
        Web::ResourceLoader::the().set_user_agent(MUST(String::from_byte_string(argument)));
        return;
    }

    if (request == "scripting") {
        page->page().set_is_scripting_enabled(argument == "on");
        return;
    }

    if (request == "block-pop-ups") {
        page->page().set_should_block_pop_ups(argument == "on");
        return;
    }

    if (request == "dump-local-storage") {
        if (auto* document = page->page().top_level_browsing_context().active_document()) {
            auto storage_or_error = document->window()->local_storage();
            if (storage_or_error.is_error())
                dbgln("Failed to retrieve local storage: {}", storage_or_error.release_error());
            else
                storage_or_error.release_value()->dump();
        }
        return;
    }

    if (request == "navigator-compatibility-mode") {
        Web::NavigatorCompatibilityMode compatibility_mode;
        if (argument == "chrome") {
            compatibility_mode = Web::NavigatorCompatibilityMode::Chrome;
        } else if (argument == "gecko") {
            compatibility_mode = Web::NavigatorCompatibilityMode::Gecko;
        } else if (argument == "webkit") {
            compatibility_mode = Web::NavigatorCompatibilityMode::WebKit;
        } else {
            dbgln("Unknown navigator compatibility mode '{}', defaulting to Chrome", argument);
            compatibility_mode = Web::NavigatorCompatibilityMode::Chrome;
        }

        Web::ResourceLoader::the().set_navigator_compatibility_mode(compatibility_mode);
        return;
    }

    if (request == "content-filtering") {
        Web::ContentFilter::the().set_filtering_enabled(argument == "on");
        return;
    }
}

void ConnectionFromClient::get_source(u64 page_id)
{
    if (auto page = this->page(page_id); page.has_value()) {
        if (auto* doc = page->page().top_level_browsing_context().active_document())
            async_did_get_source(page_id, doc->url(), doc->base_url(), doc->source());
    }
}

void ConnectionFromClient::inspect_dom_tree(u64 page_id)
{
    if (auto page = this->page(page_id); page.has_value()) {
        if (auto* doc = page->page().top_level_browsing_context().active_document())
            async_did_inspect_dom_tree(page_id, doc->dump_dom_tree_as_json());
    }
}

void ConnectionFromClient::inspect_dom_node(u64 page_id, WebView::DOMNodeProperties::Type property_type, Web::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element)
{
    auto page = this->page(page_id);
    if (!page.has_value())
        return;

    clear_inspected_dom_node(page_id);

    auto* node = Web::DOM::Node::from_unique_id(node_id);
    // Nodes without layout (aka non-visible nodes) don't have style computed.
    if (!node || !node->layout_node() || !node->is_element()) {
        async_did_inspect_dom_node(page_id, { property_type, {} });
        return;
    }

    auto& element = as<Web::DOM::Element>(*node);
    node->document().set_inspected_node(node);

    auto properties = element.computed_properties(pseudo_element);

    if (!properties) {
        async_did_inspect_dom_node(page_id, { property_type, {} });
        return;
    }

    auto serialize_computed_style = [&]() {
        JsonObject serialized;

        properties->for_each_property([&](auto property_id, auto& value) {
            serialized.set(
                Web::CSS::string_from_property_id(property_id),
                value.to_string(Web::CSS::SerializationMode::Normal));
        });

        // FIXME: Custom properties are not yet included in ComputedProperties, so add them manually.
        if (auto custom_property_data = element.custom_property_data(pseudo_element)) {
            custom_property_data->for_each_property([&](FlyString const& name, Web::CSS::StyleProperty const& value) {
                serialized.set(name, value.value->to_string(Web::CSS::SerializationMode::Normal));
            });
        }

        return serialized;
    };

    auto serialize_layout = [&](Web::Layout::Node const* layout_node) {
        if (!layout_node || !layout_node->is_box() || !layout_node->first_paintable() || !layout_node->first_paintable()->is_paintable_box()) {
            return JsonObject {};
        }

        auto const& paintable_box = as<Web::Painting::PaintableBox>(*layout_node->first_paintable());
        auto const& box_model = paintable_box.box_model();

        JsonObject serialized;

        serialized.set("width"sv, paintable_box.content_width().to_double());
        serialized.set("height"sv, paintable_box.content_height().to_double());

        serialized.set("padding-top"sv, box_model.padding.top.to_double());
        serialized.set("padding-right"sv, box_model.padding.right.to_double());
        serialized.set("padding-bottom"sv, box_model.padding.bottom.to_double());
        serialized.set("padding-left"sv, box_model.padding.left.to_double());

        serialized.set("margin-top"sv, box_model.margin.top.to_double());
        serialized.set("margin-right"sv, box_model.margin.right.to_double());
        serialized.set("margin-bottom"sv, box_model.margin.bottom.to_double());
        serialized.set("margin-left"sv, box_model.margin.left.to_double());

        serialized.set("border-top-width"sv, box_model.border.top.to_double());
        serialized.set("border-right-width"sv, box_model.border.right.to_double());
        serialized.set("border-bottom-width"sv, box_model.border.bottom.to_double());
        serialized.set("border-left-width"sv, box_model.border.left.to_double());

        serialized.set("box-sizing"sv, properties->property(Web::CSS::PropertyID::BoxSizing).to_string(Web::CSS::SerializationMode::Normal));
        serialized.set("display"sv, properties->property(Web::CSS::PropertyID::Display).to_string(Web::CSS::SerializationMode::Normal));
        serialized.set("float"sv, properties->property(Web::CSS::PropertyID::Float).to_string(Web::CSS::SerializationMode::Normal));
        serialized.set("line-height"sv, properties->property(Web::CSS::PropertyID::LineHeight).to_string(Web::CSS::SerializationMode::Normal));
        serialized.set("position"sv, properties->property(Web::CSS::PropertyID::Position).to_string(Web::CSS::SerializationMode::Normal));
        serialized.set("z-index"sv, properties->property(Web::CSS::PropertyID::ZIndex).to_string(Web::CSS::SerializationMode::Normal));

        return serialized;
    };

    auto serialize_used_fonts = [&]() {
        JsonArray serialized;

        properties->computed_font_list(node->document().font_computer())->for_each_font_entry([&](Gfx::FontCascadeList::Entry const& entry) {
            auto const& font = *entry.font;

            JsonObject font_object;
            font_object.set("name"sv, font.family().to_string());
            font_object.set("size"sv, font.point_size());
            font_object.set("weight"sv, font.weight());
            serialized.must_append(move(font_object));
        });

        return serialized;
    };

    JsonValue serialized;

    switch (property_type) {
    case WebView::DOMNodeProperties::Type::ComputedStyle:
        serialized = serialize_computed_style();
        break;
    case WebView::DOMNodeProperties::Type::Layout:
        serialized = serialize_layout(element.layout_node());
        break;
    case WebView::DOMNodeProperties::Type::UsedFonts:
        serialized = serialize_used_fonts();
        break;
    }

    async_did_inspect_dom_node(page_id, { property_type, move(serialized) });
}

void ConnectionFromClient::clear_inspected_dom_node(u64 page_id)
{
    auto page = this->page(page_id);
    if (!page.has_value())
        return;

    for (auto& navigable : Web::HTML::all_navigables()) {
        if (navigable->active_document() != nullptr) {
            navigable->active_document()->set_inspected_node(nullptr);
        }
    }
}

void ConnectionFromClient::highlight_dom_node(u64 page_id, Web::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element)
{
    auto page = this->page(page_id);
    if (!page.has_value())
        return;

    for (auto& navigable : Web::HTML::all_navigables()) {
        if (navigable->active_document() != nullptr) {
            navigable->active_document()->set_highlighted_node(nullptr, {});
        }
    }

    auto* node = Web::DOM::Node::from_unique_id(node_id);
    if (!node || !node->layout_node())
        return;

    node->document().set_highlighted_node(node, pseudo_element);
}

void ConnectionFromClient::inspect_accessibility_tree(u64 page_id)
{
    if (auto page = this->page(page_id); page.has_value()) {
        if (auto* doc = page->page().top_level_browsing_context().active_document())
            async_did_inspect_accessibility_tree(page_id, doc->dump_accessibility_tree_as_json());
    }
}

void ConnectionFromClient::get_hovered_node_id(u64 page_id)
{
    auto page = this->page(page_id);
    if (!page.has_value())
        return;

    Web::UniqueNodeID node_id = 0;

    if (auto* document = page->page().top_level_browsing_context().active_document()) {
        if (auto* hovered_node = document->hovered_node())
            node_id = hovered_node->unique_id();
    }

    async_did_get_hovered_node_id(page_id, node_id);
}

void ConnectionFromClient::list_style_sheets(u64 page_id)
{
    auto page = this->page(page_id);
    if (!page.has_value())
        return;

    async_did_list_style_sheets(page_id, page->list_style_sheets());
}

void ConnectionFromClient::request_style_sheet_source(u64 page_id, Web::CSS::StyleSheetIdentifier identifier)
{
    auto page = this->page(page_id);
    if (!page.has_value())
        return;

    if (auto* document = page->page().top_level_browsing_context().active_document()) {
        if (auto stylesheet = document->get_style_sheet_source(identifier); stylesheet.has_value())
            async_did_get_style_sheet_source(page_id, identifier, document->base_url(), stylesheet.value());
    }
}

void ConnectionFromClient::set_listen_for_dom_mutations(u64 page_id, bool listen_for_dom_mutations)
{
    auto page = this->page(page_id);
    if (!page.has_value())
        return;

    page->page().set_listen_for_dom_mutations(listen_for_dom_mutations);
}

void ConnectionFromClient::did_connect_devtools_client(u64 page_id)
{
    if (auto page = this->page(page_id); page.has_value())
        page->did_connect_devtools_client();
}

void ConnectionFromClient::did_disconnect_devtools_client(u64 page_id)
{
    auto page = this->page(page_id);
    if (!page.has_value())
        return;

    page->did_disconnect_devtools_client();
}

void ConnectionFromClient::get_dom_node_inner_html(u64 page_id, Web::UniqueNodeID node_id)
{
    auto* dom_node = Web::DOM::Node::from_unique_id(node_id);
    if (!dom_node)
        return;

    Utf16String html;

    if (dom_node->is_element()) {
        auto const& element = static_cast<Web::DOM::Element const&>(*dom_node);
        html = element.inner_html().release_value_but_fixme_should_propagate_errors().get<Utf16String>();
    } else if (dom_node->is_text() || dom_node->is_comment()) {
        auto const& character_data = static_cast<Web::DOM::CharacterData const&>(*dom_node);
        html = character_data.data();
    } else {
        return;
    }

    async_did_get_dom_node_html(page_id, html.to_utf8_but_should_be_ported_to_utf16());
}

void ConnectionFromClient::get_dom_node_outer_html(u64 page_id, Web::UniqueNodeID node_id)
{
    auto* dom_node = Web::DOM::Node::from_unique_id(node_id);
    if (!dom_node)
        return;

    Utf16String html;

    if (dom_node->is_element()) {
        auto const& element = static_cast<Web::DOM::Element const&>(*dom_node);
        html = element.outer_html().release_value_but_fixme_should_propagate_errors().get<Utf16String>();
    } else if (dom_node->is_text() || dom_node->is_comment()) {
        auto const& character_data = static_cast<Web::DOM::CharacterData const&>(*dom_node);
        html = character_data.data();
    } else {
        return;
    }

    async_did_get_dom_node_html(page_id, html.to_utf8_but_should_be_ported_to_utf16());
}

void ConnectionFromClient::set_dom_node_outer_html(u64 page_id, Web::UniqueNodeID node_id, String html)
{
    auto* dom_node = Web::DOM::Node::from_unique_id(node_id);
    if (!dom_node) {
        async_did_finish_editing_dom_node(page_id, {});
        return;
    }

    if (dom_node->is_element()) {
        auto& element = static_cast<Web::DOM::Element&>(*dom_node);
        element.set_outer_html(Utf16String::from_utf8(html)).release_value_but_fixme_should_propagate_errors();
    } else if (dom_node->is_text() || dom_node->is_comment()) {
        auto& character_data = static_cast<Web::DOM::CharacterData&>(*dom_node);
        character_data.set_data(Utf16String::from_utf8(html));
    } else {
        async_did_finish_editing_dom_node(page_id, {});
        return;
    }

    async_did_finish_editing_dom_node(page_id, node_id);
}

void ConnectionFromClient::set_dom_node_text(u64 page_id, Web::UniqueNodeID node_id, String text)
{
    auto* dom_node = Web::DOM::Node::from_unique_id(node_id);
    if (!dom_node || (!dom_node->is_text() && !dom_node->is_comment())) {
        async_did_finish_editing_dom_node(page_id, {});
        return;
    }

    auto& character_data = static_cast<Web::DOM::CharacterData&>(*dom_node);
    character_data.set_data(Utf16String::from_utf8(text));

    async_did_finish_editing_dom_node(page_id, character_data.unique_id());
}

void ConnectionFromClient::set_dom_node_tag(u64 page_id, Web::UniqueNodeID node_id, String name)
{
    auto* dom_node = Web::DOM::Node::from_unique_id(node_id);
    if (!dom_node || !dom_node->is_element() || !dom_node->parent()) {
        async_did_finish_editing_dom_node(page_id, {});
        return;
    }

    auto& element = static_cast<Web::DOM::Element&>(*dom_node);
    auto new_element = Web::DOM::create_element(element.document(), name, element.namespace_uri(), element.prefix(), element.is_value()).release_value_but_fixme_should_propagate_errors();

    element.for_each_attribute([&](auto const& attribute) {
        new_element->set_attribute_value(attribute.local_name(), attribute.value(), attribute.prefix(), attribute.namespace_uri());
    });

    while (auto* child_node = element.first_child()) {
        MUST(element.remove_child(*child_node));
        MUST(new_element->append_child(*child_node));
    }

    element.parent()->replace_child(*new_element, element).release_value_but_fixme_should_propagate_errors();
    async_did_finish_editing_dom_node(page_id, new_element->unique_id());
}

void ConnectionFromClient::add_dom_node_attributes(u64 page_id, Web::UniqueNodeID node_id, Vector<WebView::Attribute> attributes)
{
    auto* dom_node = Web::DOM::Node::from_unique_id(node_id);
    if (!dom_node || !dom_node->is_element()) {
        async_did_finish_editing_dom_node(page_id, {});
        return;
    }

    auto& element = static_cast<Web::DOM::Element&>(*dom_node);

    for (auto const& attribute : attributes) {
        // NOTE: We ignore invalid attributes for now, but we may want to send feedback to the user that this failed.
        element.set_attribute_value(attribute.name, attribute.value);
    }

    async_did_finish_editing_dom_node(page_id, element.unique_id());
}

void ConnectionFromClient::replace_dom_node_attribute(u64 page_id, Web::UniqueNodeID node_id, String name, Vector<WebView::Attribute> replacement_attributes)
{
    auto* dom_node = Web::DOM::Node::from_unique_id(node_id);
    if (!dom_node || !dom_node->is_element()) {
        async_did_finish_editing_dom_node(page_id, {});
        return;
    }

    auto& element = static_cast<Web::DOM::Element&>(*dom_node);
    bool should_remove_attribute = true;

    for (auto const& attribute : replacement_attributes) {
        if (should_remove_attribute && name.equals_ignoring_ascii_case(attribute.name))
            should_remove_attribute = false;

        // NOTE: We ignore invalid attributes for now, but we may want to send feedback to the user that this failed.
        element.set_attribute_value(attribute.name, attribute.value);
    }

    if (should_remove_attribute)
        element.remove_attribute(name);

    async_did_finish_editing_dom_node(page_id, element.unique_id());
}

void ConnectionFromClient::create_child_element(u64 page_id, Web::UniqueNodeID node_id)
{
    auto* dom_node = Web::DOM::Node::from_unique_id(node_id);
    if (!dom_node) {
        async_did_finish_editing_dom_node(page_id, {});
        return;
    }

    auto element = Web::DOM::create_element(dom_node->document(), Web::HTML::TagNames::div, Web::Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    dom_node->append_child(element).release_value_but_fixme_should_propagate_errors();

    async_did_finish_editing_dom_node(page_id, element->unique_id());
}

void ConnectionFromClient::create_child_text_node(u64 page_id, Web::UniqueNodeID node_id)
{
    auto* dom_node = Web::DOM::Node::from_unique_id(node_id);
    if (!dom_node) {
        async_did_finish_editing_dom_node(page_id, {});
        return;
    }

    auto text_node = dom_node->realm().create<Web::DOM::Text>(dom_node->document(), "text"_utf16);
    dom_node->append_child(text_node).release_value_but_fixme_should_propagate_errors();

    async_did_finish_editing_dom_node(page_id, text_node->unique_id());
}

void ConnectionFromClient::insert_dom_node_before(u64 page_id, Web::UniqueNodeID node_id, Web::UniqueNodeID parent_node_id, Optional<Web::UniqueNodeID> sibling_node_id)
{
    auto* dom_node = Web::DOM::Node::from_unique_id(node_id);
    auto* parent_dom_node = Web::DOM::Node::from_unique_id(parent_node_id);

    if (!dom_node || !parent_dom_node) {
        async_did_finish_editing_dom_node(page_id, {});
        return;
    }

    GC::Ptr<Web::DOM::Node> sibling_dom_node;
    if (sibling_node_id.has_value()) {
        sibling_dom_node = Web::DOM::Node::from_unique_id(*sibling_node_id);

        if (!sibling_dom_node) {
            async_did_finish_editing_dom_node(page_id, {});
            return;
        }
    }

    parent_dom_node->insert_before(*dom_node, sibling_dom_node);
    async_did_finish_editing_dom_node(page_id, dom_node->unique_id());
}

void ConnectionFromClient::clone_dom_node(u64 page_id, Web::UniqueNodeID node_id)
{
    auto* dom_node = Web::DOM::Node::from_unique_id(node_id);
    if (!dom_node || !dom_node->parent_node()) {
        async_did_finish_editing_dom_node(page_id, {});
        return;
    }

    auto dom_node_clone = MUST(dom_node->clone_node(nullptr, true));
    dom_node->parent_node()->insert_before(dom_node_clone, dom_node->next_sibling());

    async_did_finish_editing_dom_node(page_id, dom_node_clone->unique_id());
}

void ConnectionFromClient::remove_dom_node(u64 page_id, Web::UniqueNodeID node_id)
{
    auto page = this->page(page_id);
    if (!page.has_value())
        return;

    auto* active_document = page->page().top_level_browsing_context().active_document();
    if (!active_document) {
        async_did_finish_editing_dom_node(page_id, {});
        return;
    }

    auto* dom_node = Web::DOM::Node::from_unique_id(node_id);
    if (!dom_node) {
        async_did_finish_editing_dom_node(page_id, {});
        return;
    }

    auto* previous_dom_node = dom_node->previous_sibling();
    if (!previous_dom_node)
        previous_dom_node = dom_node->parent();

    dom_node->remove();

    async_did_finish_editing_dom_node(page_id, previous_dom_node->unique_id());
}

void ConnectionFromClient::take_document_screenshot(u64 page_id)
{
    auto page = this->page(page_id);
    if (!page.has_value())
        return;

    page->queue_screenshot_task({});
}

void ConnectionFromClient::take_dom_node_screenshot(u64 page_id, Web::UniqueNodeID node_id)
{
    auto page = this->page(page_id);
    if (!page.has_value())
        return;

    page->queue_screenshot_task(node_id);
}

static void append_page_text(Web::Page& page, StringBuilder& builder)
{
    auto* document = page.top_level_browsing_context().active_document();
    if (!document) {
        builder.append("(no DOM tree)"sv);
        return;
    }

    auto* body = document->body();
    if (!body) {
        builder.append("(no body)"sv);
        return;
    }

    builder.append(body->inner_text());
}

static void append_layout_tree(Web::Page& page, StringBuilder& builder)
{
    auto* document = page.top_level_browsing_context().active_document();
    if (!document) {
        builder.append("(no DOM tree)"sv);
        return;
    }

    document->update_layout(Web::DOM::UpdateLayoutReason::Debugging);

    auto* layout_root = document->layout_node();
    if (!layout_root) {
        builder.append("(no layout tree)"sv);
        return;
    }

    Web::dump_tree(builder, *layout_root);
}

static void append_paint_tree(Web::Page& page, StringBuilder& builder)
{
    auto* document = page.top_level_browsing_context().active_document();
    if (!document) {
        builder.append("(no DOM tree)"sv);
        return;
    }

    document->update_layout(Web::DOM::UpdateLayoutReason::Debugging);

    auto* layout_root = document->layout_node();
    if (!layout_root) {
        builder.append("(no layout tree)"sv);
        return;
    }
    if (!layout_root->first_paintable()) {
        builder.append("(no paint tree)"sv);
        return;
    }

    Web::dump_tree(builder, *layout_root->first_paintable());
}

static void append_stacking_context_tree(Web::Page& page, StringBuilder& builder)
{
    auto* document = page.top_level_browsing_context().active_document();
    if (!document) {
        builder.append("(no DOM tree)"sv);
        return;
    }

    document->update_layout(Web::DOM::UpdateLayoutReason::Debugging);

    auto* layout_root = document->layout_node();
    if (!layout_root) {
        builder.append("(no layout tree)"sv);
        return;
    }
    if (!layout_root->first_paintable()) {
        builder.append("(no paint tree)"sv);
        return;
    }

    auto& viewport_paintable = static_cast<Web::Painting::ViewportPaintable&>(*layout_root->paintable_box());
    viewport_paintable.build_stacking_context_tree_if_needed();
    if (auto* stacking_context = viewport_paintable.stacking_context()) {
        stacking_context->dump(builder);
    }
}

static void append_gc_graph(StringBuilder& builder)
{
    auto gc_graph = Web::Bindings::main_thread_vm().heap().dump_graph();
    gc_graph.serialize(builder);
}

void ConnectionFromClient::request_internal_page_info(u64 page_id, WebView::PageInfoType type)
{
    auto page = this->page(page_id);
    if (!page.has_value()) {
        async_did_get_internal_page_info(page_id, type, {});
        return;
    }

    StringBuilder builder;

    if (has_flag(type, WebView::PageInfoType::Text)) {
        append_page_text(page->page(), builder);
    }

    if (has_flag(type, WebView::PageInfoType::LayoutTree)) {
        if (!builder.is_empty())
            builder.append("\n"sv);
        append_layout_tree(page->page(), builder);
    }

    if (has_flag(type, WebView::PageInfoType::PaintTree)) {
        if (!builder.is_empty())
            builder.append("\n"sv);
        append_paint_tree(page->page(), builder);
    }

    if (has_flag(type, WebView::PageInfoType::StackingContextTree)) {
        if (!builder.is_empty())
            builder.append("\n"sv);
        append_stacking_context_tree(page->page(), builder);
    }

    if (has_flag(type, WebView::PageInfoType::GCGraph)) {
        if (!builder.is_empty())
            builder.append("\n"sv);
        append_gc_graph(builder);
    }

    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(builder.length()));
    memcpy(buffer.data<void>(), builder.string_view().characters_without_null_termination(), builder.length());
    async_did_get_internal_page_info(page_id, type, buffer);
}

Messages::WebContentServer::GetSelectedTextResponse ConnectionFromClient::get_selected_text(u64 page_id)
{
    if (auto page = this->page(page_id); page.has_value())
        return page->page().focused_navigable().selected_text().to_byte_string();
    return ByteString {};
}

void ConnectionFromClient::select_all(u64 page_id)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().focused_navigable().select_all();
}

void ConnectionFromClient::find_in_page(u64 page_id, String query, CaseSensitivity case_sensitivity)
{
    auto page = this->page(page_id);
    if (!page.has_value())
        return;

    auto result = page->page().find_in_page({ .string = query, .case_sensitivity = case_sensitivity });
    async_did_find_in_page(page_id, result.current_match_index, result.total_match_count);
}

void ConnectionFromClient::find_in_page_next_match(u64 page_id)
{
    auto page = this->page(page_id);
    if (!page.has_value())
        return;

    auto result = page->page().find_in_page_next_match();
    async_did_find_in_page(page_id, result.current_match_index, result.total_match_count);
}

void ConnectionFromClient::find_in_page_previous_match(u64 page_id)
{
    auto page = this->page(page_id);
    if (!page.has_value())
        return;

    auto result = page->page().find_in_page_previous_match();
    async_did_find_in_page(page_id, result.current_match_index, result.total_match_count);
}

void ConnectionFromClient::paste(u64 page_id, Utf16String text)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().focused_navigable().paste(text);
}

void ConnectionFromClient::set_content_filters(u64, Vector<String> filters)
{
    Web::ContentFilter::the().set_patterns(filters).release_value_but_fixme_should_propagate_errors();
}

void ConnectionFromClient::set_autoplay_allowed_on_all_websites(u64)
{
    auto& autoplay_allowlist = Web::PermissionsPolicy::AutoplayAllowlist::the();
    autoplay_allowlist.enable_globally();
}

void ConnectionFromClient::set_autoplay_allowlist(u64, Vector<String> allowlist)
{
    auto& autoplay_allowlist = Web::PermissionsPolicy::AutoplayAllowlist::the();
    autoplay_allowlist.enable_for_origins(allowlist);
}

void ConnectionFromClient::set_proxy_mappings(u64, Vector<ByteString> proxies, HashMap<ByteString, size_t> mappings)
{
    auto keys = mappings.keys();
    quick_sort(keys, [&](auto& a, auto& b) { return a.length() < b.length(); });

    OrderedHashMap<ByteString, size_t> sorted_mappings;
    for (auto& key : keys) {
        auto value = *mappings.get(key);
        if (value >= proxies.size())
            continue;
        sorted_mappings.set(key, value);
    }

    Web::ProxyMappings::the().set_mappings(move(proxies), move(sorted_mappings));
}

void ConnectionFromClient::set_preferred_color_scheme(u64 page_id, Web::CSS::PreferredColorScheme color_scheme)
{
    if (auto page = this->page(page_id); page.has_value())
        page->set_preferred_color_scheme(color_scheme);
}

void ConnectionFromClient::set_preferred_contrast(u64 page_id, Web::CSS::PreferredContrast contrast)
{
    if (auto page = this->page(page_id); page.has_value())
        page->set_preferred_contrast(contrast);
}

void ConnectionFromClient::set_preferred_motion(u64 page_id, Web::CSS::PreferredMotion motion)
{
    if (auto page = this->page(page_id); page.has_value())
        page->set_preferred_motion(motion);
}

void ConnectionFromClient::set_preferred_languages(u64, Vector<String> preferred_languages)
{
    // FIXME: Whenever the user agent needs to make the navigator.languages attribute of a Window or WorkerGlobalScope
    // object global return a new set of language tags, the user agent must queue a global task on the DOM manipulation
    // task source given global to fire an event named languagechange at global, and wait until that task begins to be
    // executed before actually returning a new value.
    Web::ResourceLoader::the().set_preferred_languages(move(preferred_languages));
}

void ConnectionFromClient::set_enable_global_privacy_control(u64, bool enable)
{
    Web::ResourceLoader::the().set_enable_global_privacy_control(enable);
}

void ConnectionFromClient::set_has_focus(u64 page_id, bool has_focus)
{
    if (auto page = this->page(page_id); page.has_value())
        page->set_has_focus(has_focus);
}

void ConnectionFromClient::set_is_scripting_enabled(u64 page_id, bool is_scripting_enabled)
{
    if (auto page = this->page(page_id); page.has_value())
        page->set_is_scripting_enabled(is_scripting_enabled);
}

void ConnectionFromClient::set_device_pixel_ratio(u64 page_id, double device_pixel_ratio)
{
    if (auto page = this->page(page_id); page.has_value())
        page->set_device_pixel_ratio(device_pixel_ratio);
}

void ConnectionFromClient::set_zoom_level(u64 page_id, double zoom_level)
{
    if (auto page = this->page(page_id); page.has_value())
        page->set_zoom_level(zoom_level);
}

void ConnectionFromClient::set_maximum_frames_per_second(u64 page_id, double maximum_frames_per_second)
{
    if (auto page = this->page(page_id); page.has_value())
        page->set_maximum_frames_per_second(maximum_frames_per_second);
}

void ConnectionFromClient::set_window_position(u64 page_id, Web::DevicePixelPoint position)
{
    if (auto page = this->page(page_id); page.has_value())
        page->set_window_position(position);
}

void ConnectionFromClient::set_window_size(u64 page_id, Web::DevicePixelSize size)
{
    if (auto page = this->page(page_id); page.has_value())
        page->set_window_size(size);
}

void ConnectionFromClient::did_update_window_rect(u64 page_id)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().did_update_window_rect();
}

void ConnectionFromClient::handle_file_return(u64, i32 error, Optional<IPC::File> file, i32 request_id)
{
    auto file_request = m_requested_files.take(request_id);

    VERIFY(file_request.has_value());
    VERIFY(file_request.value().on_file_request_finish);

    file_request.value().on_file_request_finish(error != 0 ? Error::from_errno(error) : ErrorOr<i32> { file->take_fd() });
}

void ConnectionFromClient::request_file(u64 page_id, Web::FileRequest file_request)
{
    i32 const id = last_id++;

    auto path = file_request.path();
    m_requested_files.set(id, move(file_request));

    async_did_request_file(page_id, path, id);
}

void ConnectionFromClient::set_system_visibility_state(u64 page_id, Web::HTML::VisibilityState visibility_state)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().top_level_traversable()->set_system_visibility_state(visibility_state);
}

void ConnectionFromClient::reset_zoom(u64 page_id)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().top_level_traversable()->reset_zoom();
}

void ConnectionFromClient::js_console_input(u64 page_id, String js_source)
{
    auto page = this->page(page_id);
    if (!page.has_value())
        return;

    page->js_console_input(js_source);
}

void ConnectionFromClient::run_javascript(u64 page_id, String js_source)
{
    if (auto page = this->page(page_id); page.has_value())
        page->run_javascript(js_source);
}

void ConnectionFromClient::alert_closed(u64 page_id)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().alert_closed();
}

void ConnectionFromClient::confirm_closed(u64 page_id, bool accepted)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().confirm_closed(accepted);
}

void ConnectionFromClient::prompt_closed(u64 page_id, Optional<String> response)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().prompt_closed(move(response));
}

void ConnectionFromClient::color_picker_update(u64 page_id, Optional<Color> picked_color, Web::HTML::ColorPickerUpdateState state)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().color_picker_update(picked_color, state);
}

void ConnectionFromClient::file_picker_closed(u64 page_id, Vector<Web::HTML::SelectedFile> selected_files)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().file_picker_closed(selected_files);
}

void ConnectionFromClient::select_dropdown_closed(u64 page_id, Optional<u32> selected_item_id)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().select_dropdown_closed(selected_item_id);
}

void ConnectionFromClient::retrieved_clipboard_entries(u64 page_id, u64 request_id, Vector<Web::Clipboard::SystemClipboardItem> items)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().retrieved_clipboard_entries(request_id, move(items));
}

void ConnectionFromClient::toggle_media_play_state(u64 page_id)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().toggle_media_play_state();
}

void ConnectionFromClient::toggle_media_mute_state(u64 page_id)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().toggle_media_mute_state();
}

void ConnectionFromClient::toggle_media_loop_state(u64 page_id)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().toggle_media_loop_state();
}

void ConnectionFromClient::toggle_media_controls_state(u64 page_id)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().toggle_media_controls_state();
}

void ConnectionFromClient::toggle_page_mute_state(u64 page_id)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().toggle_page_mute_state();
}

void ConnectionFromClient::set_user_style(u64 page_id, String source)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().set_user_style(move(source));
}

void ConnectionFromClient::system_time_zone_changed()
{
    JS::clear_system_time_zone_cache();
    Unicode::clear_system_time_zone_cache();
}

void ConnectionFromClient::set_document_cookie_version_buffer(u64 page_id, Core::AnonymousBuffer document_cookie_version_buffer)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().client().page_did_receive_document_cookie_version_buffer(move(document_cookie_version_buffer));
}

void ConnectionFromClient::set_document_cookie_version_index(u64 page_id, i64 document_id, Core::SharedVersionIndex document_index)
{
    if (auto page = this->page(page_id); page.has_value())
        page->page().client().page_did_receive_document_cookie_version_index(document_id, document_index);
}

void ConnectionFromClient::cookies_changed(u64 page_id, Vector<HTTP::Cookie::Cookie> cookies)
{
    if (auto page = this->page(page_id); page.has_value()) {
        auto window = page->page().top_level_traversable()->active_window();
        if (!window)
            return;

        window->cookie_store()->process_cookie_changes(move(cookies));
    }
}

// https://html.spec.whatwg.org/multipage/speculative-loading.html#nav-traversal-ui:close-a-top-level-traversable
void ConnectionFromClient::request_close(u64 page_id)
{
    // Browser user agents should offer users the ability to arbitrarily close any top-level traversable in their top-level traversable set.
    // For example, by clicking a "close tab" button.
    if (auto page = this->page(page_id); page.has_value())
        page->page().top_level_traversable()->close_top_level_traversable();
}

}
