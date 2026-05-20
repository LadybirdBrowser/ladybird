/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibURL/Pattern/Init.h>
#include <LibURL/Pattern/Pattern.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/URLPattern.h>

namespace Web::URLPattern {

using URLPatternInit = Bindings::URLPatternInit;
using URLPatternInput = Variant<String, Bindings::URLPatternInit>;
using URLPatternResult = Bindings::URLPatternResult;
using URLPatternOptions = Bindings::URLPatternOptions;

// https://urlpattern.spec.whatwg.org/#urlpattern
class URLPattern : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(URLPattern, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(URLPattern);

public:
    static WebIDL::ExceptionOr<GC::Ref<URLPattern>> create(JS::Realm&, URLPatternInput const&, Optional<String> const& base_url, URLPatternOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<URLPattern>> construct_impl(JS::Realm&, URLPatternInput const&, String const& base_url, URLPatternOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<URLPattern>> construct_impl(JS::Realm&, URLPatternInput const&, URLPatternOptions const& = {});

    WebIDL::ExceptionOr<bool> test(URLPatternInput const&, Optional<String> const& base_url) const;
    WebIDL::ExceptionOr<Optional<URLPatternResult>> exec(URLPatternInput const&, Optional<String> const& base_url) const;

    String const& protocol() const;
    String const& username() const;
    String const& password() const;
    String const& hostname() const;
    String const& port() const;
    String const& pathname() const;
    String const& search() const;
    String const& hash() const;

    bool has_reg_exp_groups() const;

    virtual ~URLPattern() override;

protected:
    virtual void initialize(JS::Realm&) override;

    explicit URLPattern(JS::Realm&, URL::Pattern::Pattern);

private:
    // https://urlpattern.spec.whatwg.org/#ref-for-url-pattern%E2%91%A0
    // Each URLPattern has an associated URL pattern, a URL pattern.
    URL::Pattern::Pattern m_url_pattern;
};

}
