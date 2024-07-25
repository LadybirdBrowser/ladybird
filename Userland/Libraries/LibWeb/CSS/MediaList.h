/*
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CSS/MediaQuery.h>

namespace Web::CSS {

// https://www.w3.org/TR/cssom-1/#the-medialist-interface
class MediaList final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(MediaList, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(MediaList);

public:
    [[nodiscard]] static JS::NonnullGCPtr<MediaList> create(JS::Realm&, Vector<NonnullRefPtr<MediaQuery>>&&);
    virtual ~MediaList() override = default;

    String media_text() const;
    void set_media_text(StringView);
    size_t length() const { return m_media.size(); }
    Optional<String> item(u32 index) const;
    void append_medium(StringView);
    void delete_medium(StringView);

    virtual Optional<JS::Value> item_value(size_t index) const override;

    bool evaluate(HTML::Window const&);
    bool matches() const;

private:
    MediaList(JS::Realm&, Vector<NonnullRefPtr<MediaQuery>>&&);

    virtual void initialize(JS::Realm&) override;

    Vector<NonnullRefPtr<MediaQuery>> m_media;
};

}
