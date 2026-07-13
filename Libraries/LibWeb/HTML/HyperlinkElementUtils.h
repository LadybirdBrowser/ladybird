/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class HyperlinkElementUtils {
public:
    virtual ~HyperlinkElementUtils();

    Utf16String origin() const;

    Utf16String protocol() const;
    void set_protocol(Utf16View);

    Utf16String username() const;
    void set_username(Utf16View);

    Utf16String password() const;
    void set_password(Utf16View);

    Utf16String host() const;
    void set_host(Utf16View);

    Utf16String hostname() const;
    void set_hostname(Utf16View);

    Utf16String port() const;
    void set_port(Utf16View);

    Utf16String pathname() const;
    void set_pathname(Utf16View);

    Utf16String search() const;
    void set_search(Utf16View);

    Utf16String hash() const;
    void set_hash(Utf16View);

protected:
    virtual DOM::Element& hyperlink_element_utils_element() = 0;
    virtual DOM::Element const& hyperlink_element_utils_element() const = 0;

    // https://html.spec.whatwg.org/multipage/links.html#update-href
    virtual void update_href() = 0;

    // https://html.spec.whatwg.org/multipage/links.html#concept-hyperlink-url-set
    virtual void set_the_url() = 0;

    Optional<URL::Origin> hyperlink_element_utils_extract_an_origin() const;

    void reinitialize_url() const;

    Optional<URL::URL> m_url;
};

}
