/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebUI/CertificateUI.h>

namespace WebView {

void CertificateUI::register_interfaces()
{
    register_interface("loadCertificateWarning"sv, [this](auto const&) {
        load_certificate_warning();
    });
    register_interface("trustCertificate"sv, [this](auto const&) {
        answer_certificate_warning(true);
    });
    register_interface("rejectCertificate"sv, [this](auto const&) {
        answer_certificate_warning(false);
    });
}

ViewImplementation* CertificateUI::view() const
{
    if (!has_client())
        return nullptr;

    auto* page = client().page(page_id());
    if (!page || !page->displays_tab())
        return nullptr;
    return &page->view();
}

void CertificateUI::load_certificate_warning()
{
    auto* view = this->view();
    if (!view || !view->certificate_warning().has_value()) {
        async_send_message("loadCertificateWarning"sv, JsonValue {});
        return;
    }

    auto const& warning = *view->certificate_warning();

    JsonObject object;
    object.set("url"sv, warning.url.serialize());
    object.set("host"sv, MUST(String::from_byte_string(warning.certificate.host)));
    object.set("fingerprint"sv, MUST(String::from_byte_string(warning.certificate.fingerprint)));
    async_send_message("loadCertificateWarning"sv, move(object));
}

void CertificateUI::answer_certificate_warning(bool trust)
{
    if (auto* view = this->view())
        view->answer_certificate_warning({}, trust);
}

}
