/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGfx/Point.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibWebCommon/HTML/PreparedNavigationDescriptor.h>
#include <LibWebCommon/Page/MediaContextMenu.h>
#include <LibWebCommon/WebView/DictionaryLookup.h>

namespace WebView {

enum class ContextMenuKind : u8 {
    Page,
    Link,
    Image,
    Media,
};

// What the user asked for a context menu on. The view finds this out, and the application decides what to offer.
struct ContextMenuRequest {
    ContextMenuKind kind { ContextMenuKind::Page };
    Gfx::IntPoint widget_position;

    bool is_editable { false };
    Optional<String> selected_text;
    Optional<DictionaryLookup> dictionary_lookup;
    Gfx::IntPoint dictionary_lookup_position;
    Optional<Utf16String> misspelled_word;
    Vector<Utf16String> spelling_guesses;

    // A link to the selected text is being made, and arrives later.
    bool makes_link_to_highlight { false };
    bool can_inspect_element { false };

    Optional<Web::HTML::PreparedNavigationDescriptor> link_navigation;
    Optional<Web::HTML::PreparedNavigationDescriptor> navigation;
    Optional<Gfx::ShareableBitmap> image_bitmap;
    Optional<String> image_mime_type;
    Optional<Web::MediaContextMenu> media;
};

enum class ContextMenuMediaAction : u8 {
    TogglePlay,
    ToggleMute,
    ToggleControls,
    ToggleLoop,
    TogglePictureInPicture,
    ToggleFullscreen,
};

}
