/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibDevTools/Actors/CSSPropertiesActor.h>
#include <LibDevTools/Actors/PageStyleActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Forward.h>
#include <LibHTTP/Cookie/Cookie.h>
#include <LibHTTP/Header.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/Forward.h>
#include <LibWeb/StorageAPI/StorageEndpoint.h>
#include <LibWebView/DOMNodeProperties.h>
#include <LibWebView/Forward.h>

namespace DevTools {

class DEVTOOLS_API DevToolsDelegate {
public:
    virtual ~DevToolsDelegate() = default;

    virtual Vector<TabDescription> tab_list() const { return {}; }
    virtual Vector<CSSProperty> css_property_list() const { return {}; }
    virtual void navigate_tab(TabDescription const&, String const&) const { }
    virtual void reload_tab(TabDescription const&, bool bypass_cache) const { (void)bypass_cache; }
    virtual void traverse_the_history_by_delta(TabDescription const&, int) const { }
    virtual Vector<HTTP::Cookie::Cookie> cookies(TabDescription const&) const { return {}; }
    virtual ErrorOr<void> set_cookie(TabDescription const&, Optional<HTTP::Cookie::Cookie>, HTTP::Cookie::Cookie) const { return {}; }
    virtual void delete_cookies(TabDescription const&, Vector<HTTP::Cookie::Cookie>) const { }
    using OnHostCookieChange = Function<void(Vector<HTTP::Cookie::Cookie>)>;
    virtual void listen_for_host_cookie_changes(TabDescription const&, OnHostCookieChange) const { }
    virtual void stop_listening_for_host_cookie_changes(TabDescription const&) const { }

    struct StorageItem {
        String name;
        String value;
    };

    using OnStorageItemsReceived = Function<void(ErrorOr<Vector<StorageItem>>)>;
    virtual void inspect_storage(TabDescription const&, Web::StorageAPI::StorageEndpointType, OnStorageItemsReceived) const { }
    virtual ErrorOr<Optional<String>> set_storage_item(TabDescription const&, Web::StorageAPI::StorageEndpointType, String const&, String const&, String const&) const { return Optional<String> {}; }
    virtual ErrorOr<Optional<String>> remove_storage_item(TabDescription const&, Web::StorageAPI::StorageEndpointType, String const&, String const&) const { return Optional<String> {}; }
    virtual ErrorOr<void> clear_storage(TabDescription const&, Web::StorageAPI::StorageEndpointType, String const&) const { return {}; }

    struct StorageChange {
        enum class Type : u8 {
            Added,
            Changed,
            Deleted,
            Cleared,
        };

        Web::StorageAPI::StorageEndpointType storage_endpoint;
        String host;
        Type type;
        Optional<String> key;
    };

    using OnStorageChange = Function<void(StorageChange)>;
    virtual u64 add_storage_change_listener(TabDescription const&, OnStorageChange) const { return 0; }
    virtual void remove_storage_change_listener(TabDescription const&, u64) const { }

    using OnTabInspectionComplete = Function<void(ErrorOr<JsonValue>)>;
    virtual void inspect_tab(TabDescription const&, OnTabInspectionComplete) const { }

    using OnAccessibilityTreeInspectionComplete = Function<void(ErrorOr<JsonValue>)>;
    virtual void inspect_accessibility_tree(TabDescription const&, OnAccessibilityTreeInspectionComplete) const { }

    using OnDOMNodePropertiesReceived = Function<void(WebView::DOMNodeProperties)>;
    virtual void listen_for_dom_properties(TabDescription const&, OnDOMNodePropertiesReceived) const { }
    virtual void stop_listening_for_dom_properties(TabDescription const&) const { }
    virtual void inspect_dom_node(TabDescription const&, WebView::DOMNodeProperties::Type, Web::UniqueNodeID, Optional<Web::CSS::PseudoElement>, JsonObject options = {}) const { (void)options; }
    virtual void clear_inspected_dom_node(TabDescription const&) const { }

    struct NodePickerEvent {
        enum class Type : u8 {
            Hovered,
            Picked,
            Previewed,
            Canceled,
        };

        Type type;
        Optional<Web::UniqueNodeID> node_id;
    };

    using OnNodePickerEvent = Function<void(NodePickerEvent)>;
    virtual void start_node_picker(TabDescription const&, OnNodePickerEvent) const { }
    virtual void stop_node_picker(TabDescription const&) const { }
    virtual void clear_node_picker(TabDescription const&) const { }

    using OnGridLayoutsReceived = Function<void(JsonArray)>;
    using OnCurrentGridReceived = Function<void(Optional<JsonObject>)>;
    using OnCurrentFlexboxReceived = Function<void(Optional<JsonObject>)>;
    virtual void inspect_grid_layouts(TabDescription const&, Web::UniqueNodeID, OnGridLayoutsReceived) const { }
    virtual void inspect_current_grid(TabDescription const&, Web::UniqueNodeID, OnCurrentGridReceived) const { }
    virtual void inspect_current_flexbox(TabDescription const&, Web::UniqueNodeID, bool, OnCurrentFlexboxReceived) const { }

    virtual void highlight_dom_node(TabDescription const&, Web::UniqueNodeID, Optional<Web::CSS::PseudoElement>) const { }
    virtual void clear_highlighted_dom_node(TabDescription const&) const { }
    virtual void highlight_flexbox(TabDescription const&, Web::UniqueNodeID, JsonValue) const { }
    virtual void clear_flexbox_highlight(TabDescription const&, Web::UniqueNodeID) const { }
    virtual void highlight_grid(TabDescription const&, Web::UniqueNodeID, JsonValue) const { }
    virtual void clear_grid_highlight(TabDescription const&, Web::UniqueNodeID) const { }

    using OnDOMMutationReceived = Function<void(WebView::Mutation)>;
    virtual void listen_for_dom_mutations(TabDescription const&, OnDOMMutationReceived) const { }
    virtual void stop_listening_for_dom_mutations(TabDescription const&) const { }

    using OnDOMNodeHTMLReceived = Function<void(ErrorOr<String>)>;
    using OnDOMNodeEditComplete = Function<void(ErrorOr<Web::UniqueNodeID>)>;
    virtual void get_dom_node_inner_html(TabDescription const&, Web::UniqueNodeID, OnDOMNodeHTMLReceived) const { }
    virtual void get_dom_node_outer_html(TabDescription const&, Web::UniqueNodeID, OnDOMNodeHTMLReceived) const { }
    virtual void set_dom_node_outer_html(TabDescription const&, Web::UniqueNodeID, String const&, OnDOMNodeEditComplete) const { }
    virtual void set_dom_node_text(TabDescription const&, Web::UniqueNodeID, String const&, OnDOMNodeEditComplete) const { }
    virtual void set_dom_node_tag(TabDescription const&, Web::UniqueNodeID, String const&, OnDOMNodeEditComplete) const { }
    virtual void add_dom_node_attributes(TabDescription const&, Web::UniqueNodeID, ReadonlySpan<WebView::Attribute>, OnDOMNodeEditComplete) const { }
    virtual void replace_dom_node_attribute(TabDescription const&, Web::UniqueNodeID, String const&, ReadonlySpan<WebView::Attribute>, OnDOMNodeEditComplete) const { }
    virtual void create_child_element(TabDescription const&, Web::UniqueNodeID, OnDOMNodeEditComplete) const { }
    virtual void insert_dom_node_before(TabDescription const&, Web::UniqueNodeID, Web::UniqueNodeID, Optional<Web::UniqueNodeID>, OnDOMNodeEditComplete) const { }
    virtual void clone_dom_node(TabDescription const&, Web::UniqueNodeID, OnDOMNodeEditComplete) const { }
    virtual void remove_dom_node(TabDescription const&, Web::UniqueNodeID, OnDOMNodeEditComplete) const { }

    using OnStyleSheetsReceived = Function<void(ErrorOr<Vector<Web::CSS::StyleSheetIdentifier>>)>;
    using OnStyleSheetSourceReceived = Function<void(Web::CSS::StyleSheetIdentifier const&, String)>;
    virtual void retrieve_style_sheets(TabDescription const&, OnStyleSheetsReceived) const { }
    virtual void retrieve_style_sheet_source(TabDescription const&, Web::CSS::StyleSheetIdentifier const&) const { }
    virtual void listen_for_style_sheet_sources(TabDescription const&, OnStyleSheetSourceReceived) const { }
    virtual void stop_listening_for_style_sheet_sources(TabDescription const&) const { }

    using OnScriptEvaluationComplete = Function<void(ErrorOr<JsonValue>)>;
    virtual void evaluate_javascript(TabDescription const&, String const&, OnScriptEvaluationComplete) const { }

    using OnConsoleMessage = Function<void(WebView::ConsoleOutput)>;
    virtual void listen_for_console_messages(TabDescription const&, OnConsoleMessage) const { }
    virtual void stop_listening_for_console_messages(TabDescription const&) const { }

    struct NetworkRequestData {
        u64 request_id { 0 };
        String url;
        String method;
        UnixDateTime start_time;
        Vector<HTTP::Header> request_headers;
        ByteBuffer request_body;
        Optional<String> initiator_type;
    };

    struct NetworkResponseData {
        u64 request_id { 0 };
        u32 status_code { 0 };
        Optional<String> reason_phrase;
        Vector<HTTP::Header> response_headers;
    };

    struct NetworkRequestCompleteData {
        u64 request_id { 0 };
        u64 body_size { 0 };
        Requests::RequestTimingInfo timing_info;
        Optional<Requests::NetworkError> network_error;
    };

    using OnNetworkRequestStarted = Function<void(NetworkRequestData)>;
    using OnNetworkResponseHeadersReceived = Function<void(NetworkResponseData)>;
    using OnNetworkResponseBodyReceived = Function<void(u64 request_id, ByteBuffer data)>;
    using OnNetworkRequestFinished = Function<void(NetworkRequestCompleteData)>;
    virtual void listen_for_network_events(TabDescription const&, OnNetworkRequestStarted, OnNetworkResponseHeadersReceived, OnNetworkResponseBodyReceived, OnNetworkRequestFinished) const { }
    virtual void stop_listening_for_network_events(TabDescription const&) const { }

    using OnNavigationStarted = Function<void(String url)>;
    using OnNavigationFinished = Function<void(String url, String title)>;
    virtual void listen_for_navigation_events(TabDescription const&, OnNavigationStarted, OnNavigationFinished) const { }
    virtual void stop_listening_for_navigation_events(TabDescription const&) const { }

    virtual void did_connect_devtools_client(TabDescription const&) const { }
    virtual void did_disconnect_devtools_client(TabDescription const&) const { }
};

}
