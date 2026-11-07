/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <UI/Qt/WebUIPage.h>

namespace Ladybird {

class VersionPage final : public WebUIPage {
public:
    explicit VersionPage(u64 web_ui_id)
        : WebUIPage(web_ui_id)
    {
    }

    virtual void received_message(String const& name, JsonValue const& data) override;

private:
    void load_version_info();
};

}
