/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/WebUI.h>

namespace WebView {

// Only about:certificate can answer the warning: the UI process assigns it a WebUI process shared with no web page.
class CertificateUI final : public WebUI {
    WEB_UI(CertificateUI);

private:
    virtual void register_interfaces() override;

    ViewImplementation* view() const;

    void load_certificate_warning();
    void answer_certificate_warning(JsonValue const&, bool trust);
};

}
