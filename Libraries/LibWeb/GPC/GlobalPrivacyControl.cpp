/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/GPC/GlobalPrivacyControl.h>
#include <LibWeb/Loader/LoaderConfig.h>

namespace Web::GlobalPrivacyControl {

GlobalPrivacyControlMixin::~GlobalPrivacyControlMixin() = default;

// https://w3c.github.io/gpc/#dom-globalprivacycontrol-globalprivacycontrol
bool GlobalPrivacyControlMixin::global_privacy_control() const
{
    // The value is false if no Sec-GPC header field would be sent; otherwise, the value is true.
    return current_loader_config()->config.enable_global_privacy_control;
}

}
