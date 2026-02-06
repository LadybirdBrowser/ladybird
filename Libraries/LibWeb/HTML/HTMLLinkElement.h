/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, Srikavin Ramkumar <me@srikavin.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/CORSSettingAttribute.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class WEB_API HTMLLinkElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLLinkElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLLinkElement);

public:
    virtual ~HTMLLinkElement() override;

    virtual void inserted() override;
    virtual void removed_from(Node* old_parent, Node& old_root) override;

    String rel() const { return get_attribute_value(HTML::AttributeNames::rel); }
    String type() const { return get_attribute_value(HTML::AttributeNames::type); }
    String href() const { return get_attribute_value(HTML::AttributeNames::href); }

    GC::Ref<DOM::DOMTokenList> rel_list();
    GC::Ref<DOM::DOMTokenList> sizes();

    bool has_loaded_icon() const;
    bool load_favicon_and_use_if_window_is_active();

    static void load_fallback_favicon_if_needed(GC::Ref<DOM::Document>);

    void set_parser_document(Badge<HTMLParser>, GC::Ref<DOM::Document> document) { m_parser_document = document; }
    void set_was_enabled_when_created_by_parser(Badge<HTMLParser>, bool was_enabled_when_created_by_parser) { m_was_enabled_when_created_by_parser = was_enabled_when_created_by_parser; }

    void set_media(String);
    String media() const;

    GC::Ptr<CSS::CSSStyleSheet> sheet() const;

    enum class AnyFailed : u8 {
        No,
        Yes,
    };
    void finished_loading_critical_style_subresources(AnyFailed);

private:
    // https://html.spec.whatwg.org/multipage/semantics.html#link-processing-options
    struct LinkProcessingOptions final : public JS::Cell {
        GC_CELL(LinkProcessingOptions, JS::Cell);
        GC_DECLARE_ALLOCATOR(LinkProcessingOptions);

        LinkProcessingOptions(
            CORSSettingAttribute crossorigin,
            ReferrerPolicy::ReferrerPolicy referrer_policy,
            URL::URL base_url,
            URL::Origin origin,
            GC::Ref<HTML::EnvironmentSettingsObject> environment,
            GC::Ref<HTML::PolicyContainer> policy_container,
            GC::Ptr<Web::DOM::Document> document,
            String cryptographic_nonce_metadata,
            Fetch::Infrastructure::Request::Priority fetch_priority);

        virtual void visit_edges(Cell::Visitor& visitor) override;

        // href (default the empty string)
        String href;

        // initiator (default "link")
        Optional<Fetch::Infrastructure::Request::InitiatorType> initiator { Fetch::Infrastructure::Request::InitiatorType::Link };

        // integrity (default the empty string)
        String integrity;

        // type (default the empty string)
        String type;

        // cryptographic nonce metadata (default the empty string)
        //     A string
        String cryptographic_nonce_metadata;

        // destination (default the empty string)
        //     A destination type.
        Optional<Fetch::Infrastructure::Request::Destination> destination;

        // crossorigin (default No CORS)
        //     A CORS settings attribute state
        CORSSettingAttribute crossorigin { CORSSettingAttribute::NoCORS };

        // referrer policy (default the empty string)
        //     A referrer policy
        ReferrerPolicy::ReferrerPolicy referrer_policy { ReferrerPolicy::ReferrerPolicy::EmptyString };

        // FIXME: source set (default null)
        //     Null or a source set

        // base URL
        //     A URL
        URL::URL base_url;

        // origin
        //     An origin
        URL::Origin origin;

        // environment
        //     An environment
        GC::Ref<HTML::EnvironmentSettingsObject> environment;

        // policy container
        //     A policy container
        GC::Ref<HTML::PolicyContainer> policy_container;

        // document (default null)
        //     Null or a Document
        GC::Ptr<Web::DOM::Document> document;

        // on document ready (default null)
        //     Null or an algorithm accepting a Document
        GC::Ptr<GC::Function<void(DOM::Document&)>> on_document_ready;

        // fetch priority (default Auto)
        //     A fetch priority attribute state
        Fetch::Infrastructure::Request::Priority fetch_priority { Fetch::Infrastructure::Request::Priority::Auto };
    };

    // https://html.spec.whatwg.org/multipage/links.html#preload-key
    struct PreloadKey {
        static PreloadKey create(Fetch::Infrastructure::Request const&);

        // URL
        //     A URL
        URL::URL url;

        // destination
        //     A string
        Optional<Fetch::Infrastructure::Request::Destination> destination;

        // mode
        //     A request mode, either "same-origin", "cors", or "no-cors"
        Fetch::Infrastructure::Request::Mode mode;

        // credentials mode
        //     A credentials mode
        Fetch::Infrastructure::Request::CredentialsMode credentials_mode;
    };

    // https://html.spec.whatwg.org/multipage/links.html#preload-entry
    struct PreloadEntry final : public JS::Cell {
        GC_CELL(PreloadEntry, JS::Cell);
        GC_DECLARE_ALLOCATOR(PreloadEntry);

        virtual void visit_edges(Cell::Visitor& visitor) override;

        // integrity metadata
        //     A string
        String integrity_metadata;

        // response
        //     Null or a response
        GC::Ptr<Fetch::Infrastructure::Response> response;

        // on response available
        //     Null, or an algorithm accepting a response or null
        GC::Ptr<GC::Function<void(GC::Ptr<Fetch::Infrastructure::Response>)>> on_response_available;
    };

    HTMLLinkElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // ^DOM::Node
    virtual bool is_html_link_element() const override { return true; }

    // ^HTMLElement
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;
    virtual bool contributes_a_script_blocking_style_sheet() const final;
    virtual bool is_implicitly_potentially_render_blocking() const override;

    GC::Ref<LinkProcessingOptions> create_link_options();
    GC::Ptr<Fetch::Infrastructure::Request> create_link_request(LinkProcessingOptions const&);

    void fetch_and_process_linked_resource();
    void default_fetch_and_process_linked_resource();
    void fetch_and_process_linked_dns_prefetch_resource();
    void fetch_and_process_linked_preconnect_resource();
    void fetch_and_process_linked_preload_resource();

    bool linked_resource_fetch_setup_steps(Fetch::Infrastructure::Request&);
    bool icon_linked_resource_fetch_setup_steps(Fetch::Infrastructure::Request&);
    bool stylesheet_linked_resource_fetch_setup_steps(Fetch::Infrastructure::Request&);

    void preconnect(LinkProcessingOptions const&);
    void preload(LinkProcessingOptions&, GC::Ptr<GC::Function<void(Fetch::Infrastructure::Response&)>> process_response = {});

    void process_linked_resource(bool success, Fetch::Infrastructure::Response const&, ByteBuffer);
    void process_icon_resource(bool success, Fetch::Infrastructure::Response const&, ByteBuffer);
    void process_stylesheet_resource(bool success, Fetch::Infrastructure::Response const&, ByteBuffer);

    bool should_fetch_and_process_resource_type() const;

    struct Relationship {
        enum {
            Alternate = 1 << 0,
            Stylesheet = 1 << 1,
            Preload = 1 << 2,
            DNSPrefetch = 1 << 3,
            Preconnect = 1 << 4,
            Icon = 1 << 5,
        };
    };

    GC::Ptr<Fetch::Infrastructure::FetchController> m_fetch_controller;
    Optional<DOM::DocumentLoadEventDelayer> m_document_load_event_delayer;

    struct LoadedIcon {
        URL::URL url;
        ByteBuffer icon;
    };

    Optional<LoadedIcon> m_loaded_icon;
    GC::Ptr<CSS::CSSStyleSheet> m_loaded_style_sheet;

    GC::Ptr<DOM::DOMTokenList> m_rel_list;
    GC::Ptr<DOM::DOMTokenList> m_sizes;
    unsigned m_relationship { 0 };

    // https://html.spec.whatwg.org/multipage/semantics.html#explicitly-enabled
    bool m_explicitly_enabled { false };

    bool m_was_enabled_when_created_by_parser { false };

    Optional<String> m_mime_type;

    GC::Weak<DOM::Document> m_parser_document;
};

}
