/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
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
#include <LibWeb/HTML/CORSSettingAttribute.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/Loader/Resource.h>

namespace Web::HTML {

class HTMLLinkElement final
    : public HTMLElement
    , public ResourceClient {
    WEB_PLATFORM_OBJECT(HTMLLinkElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLLinkElement);

public:
    virtual ~HTMLLinkElement() override;

    virtual void inserted() override;
    virtual void removed_from(Node* old_parent) override;

    String rel() const { return get_attribute_value(HTML::AttributeNames::rel); }
    String type() const { return get_attribute_value(HTML::AttributeNames::type); }
    String href() const { return get_attribute_value(HTML::AttributeNames::href); }
    String as() const;
    WebIDL::ExceptionOr<void> set_as(String const&);

    GC::Ref<DOM::DOMTokenList> rel_list();
    GC::Ref<DOM::DOMTokenList> sizes();

    bool has_loaded_icon() const;
    bool load_favicon_and_use_if_window_is_active();

    static WebIDL::ExceptionOr<void> load_fallback_favicon_if_needed(GC::Ref<DOM::Document>);

private:
    HTMLLinkElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    // ^ResourceClient
    virtual void resource_did_fail() override;
    virtual void resource_did_load() override;

    // ^DOM::Node
    virtual bool is_html_link_element() const override { return true; }

    // ^HTMLElement
    virtual void visit_edges(Cell::Visitor&) override;

    struct LinkProcessingOptions {
        // href (default the empty string)
        String href {};
        // destination (default the empty string)
        Optional<Fetch::Infrastructure::Request::Destination> destination {};
        // initiator (default "link")
        Optional<Fetch::Infrastructure::Request::InitiatorType> initiator { Fetch::Infrastructure::Request::InitiatorType::Link };
        // integrity (default the empty string)
        String integrity {};
        // type (default the empty string)
        String type {};
        // cryptographic nonce metadata (default the empty string)
        //     A string
        String cryptographic_nonce_metadata {};
        // crossorigin (default No CORS)
        //     A CORS settings attribute state
        CORSSettingAttribute crossorigin { CORSSettingAttribute::NoCORS };
        // referrer policy (default the empty string)
        //      A referrer policy
        ReferrerPolicy::ReferrerPolicy referrer_policy { ReferrerPolicy::ReferrerPolicy::EmptyString };
        // FIXME: source set (default null)
        //          Null or a source set
        // base URL
        //      A URL
        URL::URL base_url;
        // origin
        //      An origin
        URL::Origin origin;
        // environment
        //      An environment
        GC::Ptr<HTML::EnvironmentSettingsObject> environment;
        // policy container
        //      A policy container
        HTML::PolicyContainer policy_container;
        // document (default null)
        //      Null or a Document
        GC::Ptr<Web::DOM::Document> document;
        // FIXME: on document ready (default null)
        //          Null or an algorithm accepting a Document
        // fetch priority (default auto)
        //      A fetch priority attribute state
        Fetch::Infrastructure::Request::Priority fetch_priority { Fetch::Infrastructure::Request::Priority::Auto };
    };

    // https://html.spec.whatwg.org/multipage/semantics.html#create-link-options-from-element
    LinkProcessingOptions create_link_options();

    // https://html.spec.whatwg.org/multipage/semantics.html#create-a-link-request
    GC::Ptr<Fetch::Infrastructure::Request> create_link_request(LinkProcessingOptions const&);

    // https://html.spec.whatwg.org/multipage/semantics.html#linked-resource-fetch-setup-steps
    bool linked_resource_fetch_setup_steps(Fetch::Infrastructure::Request&);

    // https://html.spec.whatwg.org/multipage/links.html#link-type-stylesheet:linked-resource-fetch-setup-steps
    bool stylesheet_linked_resource_fetch_setup_steps(Fetch::Infrastructure::Request&);

    // https://html.spec.whatwg.org/multipage/semantics.html#fetch-and-process-the-linked-resource
    void fetch_and_process_linked_resource();

    // https://html.spec.whatwg.org/multipage/semantics.html#process-the-linked-resource
    void process_linked_resource(bool success, Fetch::Infrastructure::Response const&, Variant<Empty, Fetch::Infrastructure::FetchAlgorithms::ConsumeBodyFailureTag, ByteBuffer>);

    // https://html.spec.whatwg.org/multipage/links.html#link-type-stylesheet:process-the-linked-resource
    void process_stylesheet_resource(bool success, Fetch::Infrastructure::Response const&, Variant<Empty, Fetch::Infrastructure::FetchAlgorithms::ConsumeBodyFailureTag, ByteBuffer>);

    // https://html.spec.whatwg.org/multipage/semantics.html#default-fetch-and-process-the-linked-resource
    void default_fetch_and_process_linked_resource();

    void resource_did_load_favicon();

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

    GC::Ptr<CSS::CSSStyleSheet> m_loaded_style_sheet;

    Optional<DOM::DocumentLoadEventDelayer> m_document_load_event_delayer;
    GC::Ptr<DOM::DOMTokenList> m_rel_list;
    GC::Ptr<DOM::DOMTokenList> m_sizes;
    unsigned m_relationship { 0 };
    // https://html.spec.whatwg.org/multipage/semantics.html#explicitly-enabled
    bool m_explicitly_enabled { false };
};

}
