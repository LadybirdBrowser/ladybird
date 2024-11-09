/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLMediaElement.h>

namespace Web::HTML {

class HTMLAudioElement final : public HTMLMediaElement {
    WEB_PLATFORM_OBJECT(HTMLAudioElement, HTMLMediaElement);
    JS_DECLARE_ALLOCATOR(HTMLAudioElement);

public:
    virtual ~HTMLAudioElement() override;

    Layout::AudioBox* layout_node();
    Layout::AudioBox const* layout_node() const;

private:
    HTMLAudioElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual JS::GCPtr<Layout::Node> create_layout_node(CSS::StyleProperties) override;

    virtual void on_playing() override;
    virtual void on_paused() override;
    virtual void on_seek(double, MediaSeekMode) override;
    virtual void on_volume_change() override;
};

}
