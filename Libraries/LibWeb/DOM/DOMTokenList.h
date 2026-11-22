/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/DOMTokenList.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class DOMTokenList : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(DOMTokenList, dom_token_list)

public:
    [[nodiscard]] static GC::Ref<DOMTokenList> create(Element&, Utf16FlyString const& associated_attribute);

    void associated_attribute_changed(Optional<Utf16String> const&);
    void add(Utf16View);
    void remove(Utf16View);
    void toggle(Utf16View, bool force);
};

}
