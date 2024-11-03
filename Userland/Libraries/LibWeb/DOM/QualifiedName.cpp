/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <LibWeb/DOM/QualifiedName.h>

namespace Web::DOM {

static unsigned hash_impl(FlyString const& local_name, Optional<FlyString> const& prefix, Optional<FlyString> const& namespace_)
{
    unsigned hash = local_name.hash();
    if (prefix.has_value())
        hash = pair_int_hash(hash, prefix->hash());
    if (namespace_.has_value())
        hash = pair_int_hash(hash, namespace_->hash());
    return hash;
}

struct ImplTraits : public Traits<QualifiedName::Impl*> {
    static unsigned hash(QualifiedName::Impl* impl)
    {
        return hash_impl(impl->local_name, impl->prefix, impl->namespace_);
    }

    static bool equals(QualifiedName::Impl* a, QualifiedName::Impl* b)
    {
        return a->local_name == b->local_name
            && a->prefix == b->prefix
            && a->namespace_ == b->namespace_;
    }
};

static HashTable<QualifiedName::Impl*, ImplTraits> impls;

static NonnullRefPtr<QualifiedName::Impl> ensure_impl(FlyString const& local_name, Optional<FlyString> const& prefix, Optional<FlyString> const& namespace_)
{
    unsigned hash = hash_impl(local_name, prefix, namespace_);

    auto it = impls.find(hash, [&](QualifiedName::Impl* entry) {
        return entry->local_name == local_name
            && entry->prefix == prefix
            && entry->namespace_ == namespace_;
    });
    if (it != impls.end())
        return *(*it);
    return adopt_ref(*new QualifiedName::Impl(local_name, prefix, namespace_));
}

QualifiedName::QualifiedName(FlyString const& local_name, Optional<FlyString> const& prefix, Optional<FlyString> const& namespace_)
    : m_impl(ensure_impl(local_name, prefix, namespace_))
{
}

QualifiedName::Impl::Impl(FlyString const& a_local_name, Optional<FlyString> const& a_prefix, Optional<FlyString> const& a_namespace)
    : local_name(a_local_name)
    , prefix(a_prefix)
    , namespace_(a_namespace)
{
    impls.set(this);
    make_internal_string();
}

QualifiedName::Impl::~Impl()
{
    impls.remove(this);
}

// https://dom.spec.whatwg.org/#concept-attribute-qualified-name
// https://dom.spec.whatwg.org/#concept-element-qualified-name
void QualifiedName::Impl::make_internal_string()
{
    // This is possible to do according to the spec: "User agents could have this as an internal slot as an optimization."
    if (!prefix.has_value()) {
        as_string = local_name;
        return;
    }

    as_string = MUST(String::formatted("{}:{}", prefix.value(), local_name));
}

void QualifiedName::set_prefix(Optional<FlyString> value)
{
    m_impl->prefix = move(value);
}

}
