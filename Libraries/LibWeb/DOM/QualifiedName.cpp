/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/QualifiedName.h>

namespace Web::DOM {

// NB: libweb_rust defines this layout (see Rust/src/dom/qualified_name.rs), which it asserts too.
static_assert(sizeof(QualifiedName) == sizeof(void*));
static_assert(sizeof(QualifiedName::Impl) == 56 && alignof(QualifiedName::Impl) == 8);
static_assert(offsetof(QualifiedName::Impl, reference_count) == 0);
static_assert(offsetof(QualifiedName::Impl, local_name) == 8);
static_assert(offsetof(QualifiedName::Impl, lowercased_local_name) == 16);
static_assert(offsetof(QualifiedName::Impl, prefix) == 24);
static_assert(offsetof(QualifiedName::Impl, namespace_) == 32);
static_assert(offsetof(QualifiedName::Impl, as_string) == 40);
static_assert(offsetof(QualifiedName::Impl, ascii_uppercased_as_string) == 48);

static size_t raw_or_zero(Optional<Utf16FlyString> const& string)
{
    return string.has_value() ? string->raw_identity() : 0;
}

QualifiedName::QualifiedName(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& prefix, Optional<Utf16FlyString> const& namespace_)
    : m_impl(rust_dom_qualified_name_ensure(local_name.raw_identity(), raw_or_zero(prefix), raw_or_zero(namespace_)))
{
}

Utf16FlyString const& QualifiedName::ascii_uppercased_as_string() const
{
    return m_impl->ascii_uppercased_as_string.ensure([&] { return m_impl->as_string.to_ascii_uppercase(); });
}

void QualifiedName::set_prefix(Optional<Utf16FlyString> value)
{
    auto* impl = rust_dom_qualified_name_ensure(m_impl->local_name.raw_identity(), raw_or_zero(value), raw_or_zero(m_impl->namespace_));
    release(exchange(m_impl, impl));
}

}
