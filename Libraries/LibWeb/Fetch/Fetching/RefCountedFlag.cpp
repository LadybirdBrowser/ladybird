/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullRefPtr.h>
#include <LibWeb/Fetch/Fetching/RefCountedFlag.h>

namespace Web::Fetch::Fetching {

NonnullRefPtr<RefCountedFlag> RefCountedFlag::create(bool value)
{
    return adopt_ref(*new RefCountedFlag(value));
}

RefCountedFlag::RefCountedFlag(bool value)
    : m_value(value)
{
}

}
