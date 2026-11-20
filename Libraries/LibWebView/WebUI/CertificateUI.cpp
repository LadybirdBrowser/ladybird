/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibURL/Parser.h>
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
    register_interface("trustCertificate"sv, [this](auto const& data) {
        answer_certificate_warning(data, true);
    });
    register_interface("rejectCertificate"sv, [this](auto const& data) {
        answer_certificate_warning(data, false);
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
    // Use the URL to identify the answer, but display only the origin to omit credentials, paths and queries.
    object.set("url"sv, warning.url.serialize());
    object.set("origin"sv, warning.url.origin().serialize());
    object.set("resendsForm"sv, warning.form_submission.has_value());
    object.set("formIsNotResent"sv, warning.form_is_not_resent);
    object.set("host"sv, MUST(String::from_byte_string(warning.certificate.host)));
    object.set("fingerprint"sv, MUST(String::from_byte_string(warning.certificate.fingerprint)));
    async_send_message("loadCertificateWarning"sv, move(object));
}

void CertificateUI::answer_certificate_warning(JsonValue const& data, bool trust)
{
    if (!data.is_object())
        return;

    auto url_string = data.as_object().get_string("url"sv);
    auto fingerprint = data.as_object().get_string("fingerprint"sv);
    if (!url_string.has_value() || !fingerprint.has_value())
        return;

    auto url = URL::Parser::basic_parse(*url_string);
    if (!url.has_value())
        return;

    if (auto* view = this->view())
        view->answer_certificate_warning({}, *url, *fingerprint, trust);
}

}
