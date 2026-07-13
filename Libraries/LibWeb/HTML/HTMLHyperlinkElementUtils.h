/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/HyperlinkElementUtils.h>

namespace Web::HTML {

class HTMLHyperlinkElementUtils
    : public HyperlinkElementUtils {
public:
    virtual ~HTMLHyperlinkElementUtils() override;

    Utf16String href() const;
    void set_href(Utf16View);

    Utf16String target() const;
    void set_target(Utf16String);

protected:
    // ^HyperlinkElementUtils
    virtual void set_the_url() override;
    virtual void update_href() override;
};

}
