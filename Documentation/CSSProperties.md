# Adding or Modifying a CSS Property

There are several different places you need to make changes in order to add or modify a CSS property.
These are listed below in the order that Ladybird deals with them, starting at parsing and ending with them being used.

## Data

The first place you will need to go to is `CSS/Properties.json`. This file contains the definition for each
property, and is used to generate the `PropertyID` enum and a selection of functions. You may also need to
modify `CSS/Keywords.json` and `CSS/Enums.json`. See [CSSGeneratedFiles.md](CSSGeneratedFiles.md) for details.

## Parsing

For many properties, there is no need to add custom parsing code. Properties that take a single value, or shorthands
that are a list of their longhand properties, will be parsed automatically using the data in `Properties.json`.
However, there are many CSS properties with more complicated grammar and so they require custom parsing.

Property-parsing code goes in `CSS/Parser/PropertyParsing.cpp`, and `CSS/Parser/Parser.h`. First,
`Parser::parse_css_value()` is called, which has a switch for specific properties. Call your method from there. It
should return a `RefPtr` to a `CSSStyleValue` or one of its subclasses.

For shorthands, you should normally use `ShorthandStyleValue`, which automatically expands its longhand values. You
might need to modify `ShorthandStyleValue::to_string` if your shorthand has special serialization rules. For example,
`border-radius` serializes with a `/` separating the horizontal and vertical components.

If your property's value can't be represented with an existing type, you might need to add a new style value class.
If you need to do this, pester @AtkinsSJ until he gets around to documenting it. ;^)

## Computed style

After parsing and style computation, longhand properties are stored as `CSSStyleValue` pointers in
`ComputedProperties`. Any shorthands have been expanded out, and so we do not need to store them directly.

These longhands then need to be converted to a more usable form. To do this, add a getter to `ComputedProperties` with
the same name as the property. It should return a type that holds the value in a compact form. Be aware that anything
involving numbers or dimensions may be a calculation, so store it in one of the `FooOrCalculated` types.

Then, `CSS/ComputedValues.h` contains three classes that are relevant:
- `ComputedValues` holds the computed value of each property, in a flat format. Depending on whether the property is
  inherited or not, it needs adding to the `m_inherited` or `m_noninherited` structs, with a corresponding getter.
- `MutableComputedValues` also needs a setter for the value.
- `InitialValues` has a getter for the default value of the property. This isn't always needed, for example if the
  default computed value is an empty `Optional` or `Vector`.

Style is copied from `ComputedProperties` to `ComputedValues` in `NodeWithStyle::apply_style()`. Each property is
copied individually.

Then, read the value of your property with that `ComputedValues` getter we added. For example, this code reads the
computed values of `visibility` and `opacity`:

```c++
bool Paintable::is_visible() const
{
    auto const& computed_values = this->computed_values();
    return computed_values.visibility() == CSS::Visibility::Visible && computed_values.opacity() != 0;
}
```

## JavaScript

Some properties have special rules for getting the computed value from JS. For these, you will need to add to
`CSSStyleProperties::style_value_for_computed_property()`. Shorthands that are constructed in an unusual way (as in, not
using `ShorthandStyleValue`) also need handling inside `CSSStyleProperties::get_property_internal()`.
