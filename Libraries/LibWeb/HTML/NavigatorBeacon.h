/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibWeb/Fetch/BodyInit.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

class NavigatorBeaconPartial {
public:
    WebIDL::ExceptionOr<bool> send_beacon(Utf16View url, Fetch::NullableBodyInit const& data = { Empty {} });

private:
    virtual ~NavigatorBeaconPartial() = default;

    friend class Navigator;
};

}
