/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>

namespace Web::HTML {

// https://w3c.github.io/webappsec-csp/#get-csp-of-object
Optional<CSPList> retrieve_the_csp_list_of_an_object(JS::Object const& object)
{
    // 1. If object is a Document return object’s policy container's CSP list.
    if (is<DOM::Document>(object))
        return verify_cast<DOM::Document>(object).policy_container().csp_list;

    // FIXME: 2. If object is a Window or a WorkerGlobalScope or a WorkletGlobalScope, return environment settings object’s policy container's CSP list.
    //           WorkletGlobalScope not yet defined
    if (is<Window>(object))
        return verify_cast<Window>(object).associated_document().policy_container().csp_list;

    if (is<WorkerGlobalScope>(object))
        return verify_cast<WorkerGlobalScope>(object).policy_container().csp_list;

    // 3. Return null.
    return {};
}
}

namespace IPC {

template<>
ErrorOr<void> encode(IPC::Encoder& encoder, Web::HTML::PolicyContainer const& policy_container)
{
    TRY(encode(encoder, policy_container.referrer_policy));

    return {};
}

template<>
ErrorOr<Web::HTML::PolicyContainer> decode(IPC::Decoder& decoder)
{
    auto referrer_policy = TRY(decoder.decode<Web::ReferrerPolicy::ReferrerPolicy>());

    return Web::HTML::PolicyContainer { .referrer_policy = referrer_policy };
}

}
