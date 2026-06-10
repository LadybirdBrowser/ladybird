/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::EncryptedMediaExtensions {

class NavigatorEncryptedMediaExtensionsPartial {
private:
    virtual ~NavigatorEncryptedMediaExtensionsPartial() = default;

    friend class HTML::Navigator;
};

}
