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

static JsonArray certificate_error_reasons(BrowsingSession::CertificateException const& certificate)
{
    using enum Requests::CertificateError;

    JsonArray reasons;
    if (has_flag(certificate.errors, SelfSigned))
        reasons.must_append("It is self-signed, or its chain ends at a self-signed root that Ladybird doesn't trust."sv);
    if (has_flag(certificate.errors, UnknownIssuer))
        reasons.must_append("It comes from an issuer that Ladybird doesn't know or doesn't trust."sv);
    if (has_flag(certificate.errors, Expired))
        reasons.must_append("It has expired. If the date and time of this computer are wrong, correct them instead."sv);
    if (has_flag(certificate.errors, NotYetValid))
        reasons.must_append("It isn't valid yet. If the date and time of this computer are wrong, correct them instead."sv);
    if (has_flag(certificate.errors, NameMismatch))
        reasons.must_append(MUST(String::formatted("It was issued for another site, not for {}.", certificate.host)));
    if (has_flag(certificate.errors, Other)) {
        if (certificate.error_details.is_empty())
            reasons.must_append("It failed verification."sv);
        else
            reasons.must_append(MUST(String::formatted("It failed verification: {}.", certificate.error_details)));
    }
    return reasons;
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
    object.set("reasons"sv, certificate_error_reasons(warning.certificate));
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
