/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::GlobalPrivacyControl {

// https://w3c.github.io/gpc/#dom-globalprivacycontrol
class GlobalPrivacyControlMixin {
public:
    virtual ~GlobalPrivacyControlMixin();

    bool global_privacy_control() const;
};

}
