/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaListPrototype.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/MediaList.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Dump.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(MediaList);

GC::Ref<MediaList> MediaList::create(JS::Realm& realm, Vector<NonnullRefPtr<MediaQuery>>&& media)
{
    return realm.create<MediaList>(realm, move(media));
}

MediaList::MediaList(JS::Realm& realm, Vector<NonnullRefPtr<MediaQuery>>&& media)
    : Bindings::PlatformObject(realm)
    , m_media(move(media))
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags { .supports_indexed_properties = true };
}

void MediaList::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaList);
    Base::initialize(realm);
}

void MediaList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_associated_style_sheet);
}

// https://www.w3.org/TR/cssom-1/#dom-medialist-mediatext
String MediaList::media_text() const
{
    return serialize_a_media_query_list(m_media);
}

// https://www.w3.org/TR/cssom-1/#dom-medialist-mediatext
void MediaList::set_media_text(StringView text)
{
    ScopeGuard guard = [&] {
        if (m_associated_style_sheet)
            as<CSS::CSSStyleSheet>(*m_associated_style_sheet).invalidate_owners(DOM::StyleInvalidationReason::MediaListSetMediaText);
    };

    m_media.clear();
    if (text.is_empty())
        return;
    m_media = parse_media_query_list(Parser::ParsingParams { realm() }, text);
}

// https://www.w3.org/TR/cssom-1/#dom-medialist-item
Optional<String> MediaList::item(u32 index) const
{
    if (index >= m_media.size())
        return {};

    return m_media[index]->to_string();
}

// https://www.w3.org/TR/cssom-1/#dom-medialist-appendmedium
void MediaList::append_medium(StringView medium)
{
    // 1. Let m be the result of parsing the given value.
    auto m = parse_media_query(Parser::ParsingParams { realm() }, medium);

    // 2. If m is null, then return.
    if (!m)
        return;

    // 3. If comparing m with any of the media queries in the collection of media queries returns true, then return.
    auto serialized = m->to_string();
    for (auto& existing_medium : m_media) {
        if (existing_medium->to_string() == serialized)
            return;
    }

    // 4. Append m to the collection of media queries.
    m_media.append(m.release_nonnull());

    if (m_associated_style_sheet)
        as<CSS::CSSStyleSheet>(*m_associated_style_sheet).invalidate_owners(DOM::StyleInvalidationReason::MediaListAppendMedium);
}

// https://www.w3.org/TR/cssom-1/#dom-medialist-deletemedium
WebIDL::ExceptionOr<void> MediaList::delete_medium(StringView medium)
{
    // 1. Let m be the result of parsing the given value.
    auto m = parse_media_query(Parser::ParsingParams { realm() }, medium);

    // 2. If m is null, then return.
    if (!m)
        return {};

    // 3. Remove any media query from the collection of media queries for which comparing the media query with m
    //    returns true. If nothing was removed, then throw a NotFoundError exception.
    bool was_removed = m_media.remove_all_matching([&](auto& existing) -> bool {
        return m->to_string() == existing->to_string();
    });
    if (!was_removed)
        return WebIDL::NotFoundError::create(realm(), "Media query not found in list"_utf16);

    if (m_associated_style_sheet)
        as<CSS::CSSStyleSheet>(*m_associated_style_sheet).invalidate_owners(DOM::StyleInvalidationReason::MediaListDeleteMedium);

    return {};
}

bool MediaList::evaluate(DOM::Document const& document)
{
    for (auto& media : m_media)
        media->evaluate(document);

    return matches();
}

bool MediaList::matches() const
{
    if (m_media.is_empty())
        return true;

    for (auto& media : m_media) {
        if (media->matches())
            return true;
    }
    return false;
}

Optional<JS::Value> MediaList::item_value(size_t index) const
{
    if (index >= m_media.size())
        return {};
    return JS::PrimitiveString::create(vm(), m_media[index]->to_string());
}

void MediaList::dump(StringBuilder& builder, int indent_levels) const
{
    dump_indent(builder, indent_levels);
    builder.appendff("Media list ({}):\n", m_media.size());
    for (auto const& media : m_media)
        media->dump(builder, indent_levels + 1);
}

}
