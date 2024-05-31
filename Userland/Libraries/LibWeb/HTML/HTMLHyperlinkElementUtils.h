/*
 * Copyright (c) 2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/TokenizedFeatures.h>

namespace Web::HTML {

class HTMLHyperlinkElementUtils {
public:
    virtual ~HTMLHyperlinkElementUtils();

    String origin() const;

    String href() const;
    WebIDL::ExceptionOr<void> set_href(String);

    String protocol() const;
    void set_protocol(StringView);

    String username() const;
    void set_username(StringView);

    String password() const;
    void set_password(StringView);

    String host() const;
    void set_host(StringView);

    String hostname() const;
    void set_hostname(StringView);

    String port() const;
    void set_port(StringView);

    String pathname() const;
    void set_pathname(StringView);

    String search() const;
    void set_search(StringView);

    String hash() const;
    void set_hash(StringView);

protected:
    virtual DOM::Document& hyperlink_element_utils_document() = 0;
    virtual Optional<String> hyperlink_element_utils_href() const = 0;
    virtual WebIDL::ExceptionOr<void> set_hyperlink_element_utils_href(String) = 0;
    virtual Optional<String> hyperlink_element_utils_referrerpolicy() const = 0;
    virtual bool hyperlink_element_utils_is_html_anchor_element() const = 0;
    virtual bool hyperlink_element_utils_is_connected() const = 0;
    virtual String hyperlink_element_utils_get_an_elements_target() const = 0;
    virtual TokenizedFeature::NoOpener hyperlink_element_utils_get_an_elements_noopener(StringView target) const = 0;

    virtual void hyperlink_element_utils_queue_an_element_task(HTML::Task::Source source, Function<void()> steps) = 0;

    void set_the_url();
    void follow_the_hyperlink(Optional<String> hyperlink_suffix, UserNavigationInvolvement = UserNavigationInvolvement::None);

private:
    void reinitialize_url() const;
    void update_href();
    bool cannot_navigate() const;

    Optional<URL::URL> m_url;
};

}
