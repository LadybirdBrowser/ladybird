/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Fetch/BodyInit.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

class NavigatorBeaconMixin {
public:
    WebIDL::ExceptionOr<bool> send_beacon(String const& url, Optional<Fetch::BodyInit> const& data = {});

private:
    virtual ~NavigatorBeaconMixin() = default;

    friend class Navigator;
};

}
