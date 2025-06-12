/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>

namespace Web::Fetch::Fetching {

/// A ref-counted boolean flag.
/// This is used to share flags between multiple callback closures.
class RefCountedFlag : public RefCounted<RefCountedFlag> {
public:
    static NonnullRefPtr<RefCountedFlag> create(bool);

    [[nodiscard]] bool value() const { return m_value; }
    void set_value(bool value) { m_value = value; }

private:
    explicit RefCountedFlag(bool);

    bool m_value { false };
};

}
