/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StdLibExtras.h>
#include <AK/Utf16FlyString.h>
#include <LibWeb/Export.h>

namespace Web::DOM {

class WEB_API QualifiedName {
public:
    QualifiedName(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& prefix, Optional<Utf16FlyString> const& namespace_);

    QualifiedName(QualifiedName const& other)
        : m_impl(other.m_impl)
    {
        ++m_impl->reference_count;
    }

    QualifiedName(QualifiedName&& other)
        : m_impl(exchange(other.m_impl, nullptr))
    {
    }

    QualifiedName& operator=(QualifiedName const& other)
    {
        if (this != &other) {
            ++other.m_impl->reference_count;
            release(exchange(m_impl, other.m_impl));
        }
        return *this;
    }

    QualifiedName& operator=(QualifiedName&& other)
    {
        if (this != &other)
            release(exchange(m_impl, exchange(other.m_impl, nullptr)));
        return *this;
    }

    ~QualifiedName() { release(m_impl); }

    Utf16FlyString const& local_name() const { return m_impl->local_name; }
    Optional<Utf16FlyString> const& prefix() const { return m_impl->prefix; }
    Optional<Utf16FlyString> const& namespace_() const { return m_impl->namespace_; }

    Utf16FlyString const& lowercased_local_name() const { return m_impl->lowercased_local_name; }

    Utf16FlyString const& as_string() const { return m_impl->as_string; }
    Utf16FlyString const& ascii_uppercased_as_string() const;

    // The interned parts of a name, which libweb_rust allocates, interns and frees, and whose layout it defines (see
    // Rust/src/dom/qualified_name.rs). C++ never constructs or destroys one: it only counts its references, and frees
    // it through Rust as it drops the last of them.
    struct Impl {
        Impl() = delete;
        ~Impl() = delete;

        u32 reference_count;
        Utf16FlyString local_name;
        Utf16FlyString lowercased_local_name;
        Optional<Utf16FlyString> prefix;
        Optional<Utf16FlyString> namespace_;
        Utf16FlyString as_string;
        mutable Optional<Utf16FlyString> ascii_uppercased_as_string;
    };

    // Names that are equal in every part share their Impl, so this tells them apart in one comparison.
    // A name whose parts are these, which libweb_rust holds a reference to (see Rust/src/dom/qualified_name.rs).
    explicit QualifiedName(Impl& impl)
        : m_impl(&impl)
    {
        ++impl.reference_count;
    }

    Impl const* identity() const { return m_impl; }

    void set_prefix(Optional<Utf16FlyString> value);

private:
    static void release(Impl*);

    Impl* m_impl { nullptr };
};

// The interning of qualified names, which libweb_rust does (see Rust/src/dom/qualified_name.rs).
extern "C" {
// The parts of the name whose local name, prefix and namespace are the strings whose raw words are local_name, prefix
// and namespace_ (0 for null), with a reference of the caller's own to them.
QualifiedName::Impl* rust_dom_qualified_name_ensure(size_t local_name, size_t prefix, size_t namespace_);
// Frees the parts of a name, which no one holds a reference to any longer.
void rust_dom_qualified_name_free(QualifiedName::Impl*);
}

inline void QualifiedName::release(Impl* impl)
{
    if (impl && --impl->reference_count == 0)
        rust_dom_qualified_name_free(impl);
}

}
