/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/HyperlinkElementUtils.h>

namespace Web::HTML {

class HTMLHyperlinkElementUtils
    : public HyperlinkElementUtils {
public:
    virtual ~HTMLHyperlinkElementUtils() override;

    String href() const;
    void set_href(String);

    String target() const;
    void set_target(String);

protected:
    // ^HyperlinkElementUtils
    virtual void set_the_url() override;
    virtual void update_href() override;
};

}
