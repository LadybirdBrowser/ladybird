/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/POSTResource.h>
#include <LibWeb/HTML/SerializedPolicyContainer.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state-2
class DocumentState final : public RefCounted<DocumentState> {
public:
    static NonnullRefPtr<DocumentState> create() { return adopt_ref(*new DocumentState()); }
    ~DocumentState();

    struct NestedHistory {
        String id;
        Vector<NonnullRefPtr<SessionHistoryEntry>> entries;
    };

    enum class Client {
        Tag,
    };

    [[nodiscard]] Optional<UniqueNodeID> document_id() const { return m_document_id; }
    void set_document_id(Optional<UniqueNodeID> document_id) { m_document_id = document_id; }

    [[nodiscard]] Variant<SerializedPolicyContainer, Client> const& history_policy_container() const { return m_history_policy_container; }
    void set_history_policy_container(Variant<SerializedPolicyContainer, Client> history_policy_container) { m_history_policy_container = move(history_policy_container); }

    [[nodiscard]] Fetch::Infrastructure::Request::ReferrerType request_referrer() const { return m_request_referrer; }
    void set_request_referrer(Fetch::Infrastructure::Request::ReferrerType request_referrer) { m_request_referrer = move(request_referrer); }

    [[nodiscard]] ReferrerPolicy::ReferrerPolicy request_referrer_policy() const { return m_request_referrer_policy; }
    void set_request_referrer_policy(ReferrerPolicy::ReferrerPolicy request_referrer_policy) { m_request_referrer_policy = move(request_referrer_policy); }

    [[nodiscard]] Optional<URL::Origin> initiator_origin() const { return m_initiator_origin; }
    void set_initiator_origin(Optional<URL::Origin> initiator_origin) { m_initiator_origin = move(initiator_origin); }

    [[nodiscard]] Optional<URL::Origin> origin() const { return m_origin; }
    void set_origin(Optional<URL::Origin> origin) { m_origin = move(origin); }

    [[nodiscard]] Optional<URL::URL> const& about_base_url() const { return m_about_base_url; }
    void set_about_base_url(Optional<URL::URL> url) { m_about_base_url = move(url); }

    [[nodiscard]] Vector<NestedHistory> const& nested_histories() const { return m_nested_histories; }
    [[nodiscard]] Vector<NestedHistory>& nested_histories() { return m_nested_histories; }

    [[nodiscard]] Variant<Empty, String, POSTResource> resource() const { return m_resource; }
    void set_resource(Variant<Empty, String, POSTResource> resource) { m_resource = move(resource); }

    [[nodiscard]] bool reload_pending() const { return m_reload_pending; }
    void set_reload_pending(bool reload_pending) { m_reload_pending = reload_pending; }

    [[nodiscard]] bool ever_populated() const { return m_ever_populated; }
    void set_ever_populated(bool ever_populated) { m_ever_populated = ever_populated; }

    [[nodiscard]] String navigable_target_name() const { return m_navigable_target_name; }
    void set_navigable_target_name(String navigable_target_name) { m_navigable_target_name = navigable_target_name; }

private:
    DocumentState();

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state-document
    // NOTE: We store the document's unique ID rather than a pointer to the document, because DocumentState is
    //       decoupled from the document's lifetime (Navigable owns the document directly).
    Optional<UniqueNodeID> m_document_id;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state-history-policy-container
    Variant<SerializedPolicyContainer, Client> m_history_policy_container { Client::Tag };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state-request-referrer
    Fetch::Infrastructure::Request::ReferrerType m_request_referrer { Fetch::Infrastructure::Request::Referrer::Client };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state-request-referrer-policy
    ReferrerPolicy::ReferrerPolicy m_request_referrer_policy { ReferrerPolicy::DEFAULT_REFERRER_POLICY };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state-initiator-origin
    Optional<URL::Origin> m_initiator_origin;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state-origin
    Optional<URL::Origin> m_origin;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state-about-base-url
    Optional<URL::URL> m_about_base_url = {};

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state-nested-histories
    Vector<NestedHistory> m_nested_histories;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state-resource
    Variant<Empty, String, POSTResource> m_resource {};

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state-reload-pending
    bool m_reload_pending { false };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state-ever-populated
    bool m_ever_populated { false };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state-nav-target-name
    String m_navigable_target_name;
};

}
