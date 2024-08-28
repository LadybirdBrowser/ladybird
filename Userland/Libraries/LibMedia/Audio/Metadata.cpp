/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Metadata.h"
#include <AK/Assertions.h>
#include <LibCore/Version.h>

namespace Audio {

bool Person::is_artist() const
{
    return role == Person::Role::Artist
        || role == Person::Role::Composer
        || role == Person::Role::Conductor
        || role == Person::Role::Lyricist
        || role == Person::Role::Performer;
}

Optional<StringView> Person::name_for_role() const
{
    switch (role) {
    case Role::Artist:
    case Role::Performer:
        return {};
    case Role::Lyricist:
        return "Lyricist"sv;
    case Role::Conductor:
        return "Conductor"sv;
    case Role::Publisher:
        return "Publisher"sv;
    case Role::Engineer:
        return "Engineer"sv;
    case Role::Composer:
        return "Composer"sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<String> Metadata::first_artist() const
{
    auto artist = people.find_if([](auto const& person) { return person.is_artist(); });
    if (artist.is_end())
        return {};
    return artist->name;
}

ErrorOr<Optional<String>> Metadata::all_artists(StringView concatenate_with) const
{
    // FIXME: This entire function could be similar to TRY(TRY(people.filter(...).try_map(...)).join(concatenate_with)) if these functional iterator transformers existed :^)
    Vector<String> artist_texts;
    TRY(artist_texts.try_ensure_capacity(people.size()));
    for (auto const& person : people) {
        if (!person.is_artist())
            continue;
        if (auto role_name = person.name_for_role(); role_name.has_value())
            artist_texts.unchecked_append(TRY(String::formatted("{} ({})", person.name, role_name.release_value())));
        else
            artist_texts.unchecked_append(person.name);
    }
    if (artist_texts.is_empty())
        return Optional<String> {};
    return String::join(concatenate_with, artist_texts);
}

ErrorOr<void> Metadata::add_miscellaneous(String const& field, String value)
{
    // FIXME: Since try_ensure does not return a reference to the contained value, we have to retrieve it separately.
    //        This is a try_ensure bug that should be fixed.
    (void)TRY(miscellaneous.try_ensure(field, []() { return Vector<String> {}; }));
    auto& values_for_field = miscellaneous.get(field).release_value();
    return values_for_field.try_append(move(value));
}

ErrorOr<void> Metadata::add_person(Person::Role role, String name)
{
    return people.try_append(Person { role, move(name) });
}

}
