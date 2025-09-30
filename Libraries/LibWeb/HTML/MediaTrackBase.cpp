/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/Locale.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/MediaTrackBase.h>

namespace Web::HTML {

MediaTrackBase::MediaTrackBase(JS::Realm& realm, GC::Ref<HTMLMediaElement> media_element, Media::Track const& track)
    : PlatformObject(realm)
    , m_media_element(media_element)
    , m_track_in_playback_manager(track)
    , m_id(Utf16String::number(track.identifier()))
    , m_label(track.name())
{
    // https://html.spec.whatwg.org/multipage/media.html#dom-audiotrack-language
    // https://html.spec.whatwg.org/multipage/media.html#dom-videotrack-language
    // The AudioTrack language and VideoTrack language attributes must return the BCP 47 language tag of the language
    // of the track, if it has one, or the empty string otherwise. If the user agent is not able to express that language
    // as a BCP 47 language tag (for example because the language information in the media resource's format is a free-form
    // string without a defined interpretation), then the method must return the empty string, as if the track had no
    // language.
    m_language = [&] {
        auto locale = Unicode::parse_unicode_locale_id(track.language().to_utf8());
        if (!locale.has_value())
            return Utf16String();
        auto language = locale->to_string();
        // NOTE: We specifically want to exclude "und" here, as RFC 5646 says:
        //
        //     The 'und' (Undetermined) primary language subtag identifies linguistic content whose language is not
        //     determined. This subtag SHOULD NOT be used unless a language tag is required and language information is
        //     not available or cannot be determined.  Omitting the language tag (where permitted) is preferred.  The 'und'
        //     subtag might be useful for protocols that require a language tag to be provided or where a primary language
        //     subtag is required (such as in "und-Latn").  The 'und' subtag MAY also be useful when matching language tags
        //     in certain situations.
        //
        // Matroska's TrackEntry->Language element is required, and will use "und" as a placeholder as mentioned above. We
        // don't want to return anything when that placeholder is found:
        if (language == "und")
            return Utf16String();
        return Utf16String::from_utf8(language);
    }();
}

MediaTrackBase::~MediaTrackBase() = default;

void MediaTrackBase::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_element);
}

}
