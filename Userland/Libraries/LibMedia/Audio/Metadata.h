/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/LexicalPath.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Variant.h>

namespace Audio {

struct Person {
    enum class Role {
        Artist,
        Performer,
        Lyricist,
        Conductor,
        Publisher,
        Engineer,
        Composer,
    };
    Role role;
    String name;

    // Whether this person has creative involvement with the song (so not only Role::Artist!).
    // This list is subjective and is intended to keep the artist display text in applications relevant.
    // It is used for first_artist and all_artists in Metadata.
    bool is_artist() const;

    Optional<StringView> name_for_role() const;
};

// Audio metadata of the original format must be equivalently reconstructible from this struct.
// That means, (if the format allows it) fields can appear in a different order, but all fields must be present with the original values,
// including duplicate fields where allowed by the format.
struct Metadata {
    using Year = unsigned;

    void replace_encoder_with_serenity();
    ErrorOr<void> add_miscellaneous(String const& field, String value);
    ErrorOr<void> add_person(Person::Role role, String name);
    Optional<String> first_artist() const;
    ErrorOr<Optional<String>> all_artists(StringView concatenate_with = ", "sv) const;

    Optional<String> title;
    Optional<String> subtitle;
    Optional<unsigned> track_number;
    Optional<String> album;
    Optional<String> genre;
    Optional<String> comment;
    Optional<String> isrc;
    Optional<String> encoder;
    Optional<String> copyright;
    Optional<float> bpm;
    // FIXME: Until the time data structure situation is solved in a good way, we don't parse ISO 8601 time specifications.
    Optional<String> unparsed_time;
    Vector<Person> people;

    // Any other metadata, using the format-specific field names. This ensures reproducibility.
    HashMap<String, Vector<String>> miscellaneous;
};

}
