/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

// NB: libweb_rust defines, allocates and destroys every Comment, see Rust/src/dom/node_objects.rs.
class Comment final : public CharacterData {
    WEB_WRAPPABLE(Comment, CharacterData);

public:
    [[nodiscard]] static GC::Ref<Comment> create(Document&, Utf16String data);
    [[nodiscard]] static GC::Ref<Comment> create_for_constructor(JS::Object&, Utf16String data);

    virtual Utf16FlyString node_name() const override { return "#comment"_utf16_fly_string; }

private:
    friend struct RustNodeObjectHooks;

    explicit Comment(VtableProbe);
    virtual ~Comment() override = default;
};

}
