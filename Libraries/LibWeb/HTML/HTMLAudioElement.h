/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLMediaElement.h>

namespace Web::HTML {

class HTMLAudioElement final : public HTMLMediaElement {
    WEB_WRAPPABLE(HTMLAudioElement, HTMLMediaElement);

public:
    virtual ~HTMLAudioElement() override;

    bool should_paint() const;

private:
    explicit HTMLAudioElement(DOM::RustMadePrefix);
    virtual CSS::ElementBoxKind box_kind() const override;
};

}
