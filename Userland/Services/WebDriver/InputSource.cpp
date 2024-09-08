/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <WebDriver/InputSource.h>
#include <WebDriver/InputState.h>

namespace Web::WebDriver {

// To get a pointer id given input state and subtype :
static unsigned get_a_pointer_id(InputState const& input_state, String const& subtype)
{
    // 1. Let minimum id be 0 if subtype is "mouse", or 2 otherwise.
    unsigned minimum_id = (subtype == "mouse") ? 0 : 2;

    // 2. Let pointer ids be an empty set.
    HashTable<unsigned> pointer_ids;

    // 3. Let sources be the result of getting the values with input state's input state map.
    // Does the HashMap only have this function for keys? Could implement for values
    Vector<NonnullOwnPtr<InputSource>> sources;
    for (auto const& [_, v] : input_state.get_input_state_map())
        sources.append(v);

    // FIXME: 4. For each source in sources.:
    for (auto const& source : sources) {
        //     FIXME: 1. If source is a pointer input source, append source's pointerId to pointer ids.
        //     FIXME: Input id is a UUID, which we treat as strings
        //     Here it's necessary to treat them as integers,
        //     FIXME: How to switch on the type inside the pointer?

        if (source->input_id() == "pointer") // this condition is wrong
            pointer_ids.set(source->input_id());
    }

    // FIXME: 5. Return the smallest integer that is greater than or equal to minimum id and that is not contained in pointer ids.
    // This would be easier to implement if we heap pushed into pointer_ids

    return minimum_id; // placeholder
}

// FIXME: Check return type
static ErrorOr<NonnullOwnPtr<Web::WebDriver::InputSource>> create(InputState const& input_state, String const& type,
    String const& subtype)
{
    // FIXME: The InputSource as the template parameter was giving no matching
    // ctor errors
    NonnullOwnPtr<InputSource> source = make<NullInputSource>();
    // 1. Run the substeps matching the first matching value of type:
    //      "none"
    //          Let source be the result of create a null input source.
    if (type == "none")
        source = NullInputSource::create();
    //      "key"
    //          Let source be the result of create a key input source.
    else if (type == "key")
        source = KeyInputSource::create();
    //      "pointer"
    //          Let source be the result of create a pointer input source with input state and subtype.
    else if (type == "pointer")
        source = PointerInputSource::create(input_state, subtype);
    //      "wheel"
    //          Let source be the result of create a wheel input source.
    else if (type == "wheel")
        source = WheelInputSource::create();
    //      FIXME: Otherwise:
    //          Return error with error code invalid argument.
    // else
    // return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument);

    // 2. Return success with data source.
    return source;
}

NonnullOwnPtr<NullInputSource> NullInputSource::create()
{
    return adopt_own(*new NullInputSource);
}
NonnullOwnPtr<KeyInputSource> KeyInputSource::create()
{
    return adopt_own(*new KeyInputSource);
}
NonnullOwnPtr<PointerInputSource> PointerInputSource::create(InputState const& input_state, String const& subtype)
{
    return adopt_own(*new PointerInputSource(input_state, subtype));
}
NonnullOwnPtr<WheelInputSource> WheelInputSource::create()
{

    return adopt_own(*new WheelInputSource);
}

PointerInputSource::PointerInputSource(InputState const& input_state, String const& subtype)
    : m_subtype(subtype)
    , m_pointer_id(get_a_pointer_id(input_state, subtype))
{
}

}
