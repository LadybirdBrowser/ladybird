/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <LibWeb/Export.h>

namespace Web::DOM {

class WEB_API QualifiedName {
public:
    QualifiedName(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& prefix, Optional<Utf16FlyString> const& namespace_);

    Utf16FlyString const& local_name() const { return m_impl->local_name; }
    Optional<Utf16FlyString> const& prefix() const { return m_impl->prefix; }
    Optional<Utf16FlyString> const& namespace_() const { return m_impl->namespace_; }

    Utf16FlyString const& lowercased_local_name() const { return m_impl->lowercased_local_name; }

    Utf16FlyString const& as_string() const { return m_impl->as_string; }

    struct WEB_API Impl : public RefCounted<Impl> {
        Impl(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& prefix, Optional<Utf16FlyString> const& namespace_);
        ~Impl();

        void make_internal_string();
        Utf16FlyString local_name;
        Utf16FlyString lowercased_local_name;
        Optional<Utf16FlyString> prefix;
        Optional<Utf16FlyString> namespace_;
        Utf16FlyString as_string;
    };

    void set_prefix(Optional<Utf16FlyString> value);

private:
    NonnullRefPtr<Impl> m_impl;
};

}
