# CSS Generated Files

We generate a significant amount of CSS-related code, taking in one or more .json files in
[`Libraries/LibWeb/CSS`](../Libraries/LibWeb/CSS) and producing C++ code from them, located in
`Build/<build-preset>/Lagom/Libraries/LibWeb/CSS/`.
It's likely that you'll need to work with these if you add or modify a CSS property or its values.

The generators are found in [`Meta/Lagom/Tools/CodeGenerators/LibWeb`](../Meta/Lagom/Tools/CodeGenerators/LibWeb).
They are run automatically as part of the build, and most of the time you can ignore them.

## Properties.json

Each CSS property has an entry here, which describes what values it accepts, whether it's inherited, and similar data.
This generates `PropertyID.h`, `PropertyID.cpp`, `GeneratedCSSStyleProperties.h`, `GeneratedCSSStyleProperties.cpp` and `GeneratedCSSStyleProperties.idl`.
Most of this data is found in the information box for that property in the relevant CSS spec.

The file is organized as a single JSON object, with keys being property names, and the values being the data for that property.
Each property will have some set of these fields on it:

(Note that required fields are not required on properties with `legacy-alias-for` or `logical-alias-for` set.)

| Field                      | Required | Default | Description                                                                                                                               | Generated functions                                                         |
|----------------------------|----------|---------|-------------------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------|
| `affects-layout`           | No       | `true`  | Boolean. Whether changing this property will invalidate the element's layout.                                                             | `bool property_affects_layout(PropertyID)`                                  |
| `affects-stacking-context` | No       | `false` | Boolean. Whether this property can cause a new stacking context for the element.                                                          | `bool property_affects_stacking_context(PropertyID)`                        |
| `animation-type`           | Yes      |         | String. How the property should be animated. Defined by the spec. See below.                                                              | `AnimationType animation_type_from_longhand_property(PropertyID)`           |
| `inherited`                | Yes      |         | Boolean. Whether the property is inherited by its child elements.                                                                         | `bool is_inherited_property(PropertyID)`                                    |
| `initial`                  | Yes      |         | String. The property's initial value if it is not specified.                                                                              | `NonnullRefPtr<CSSStyleValue> property_initial_value(PropertyID)`           |
| `legacy-alias-for`         | No       | Nothing | String. The name of a property this is an alias for. See below.                                                                           |                                                                             |
| `logical-alias-for`        | No       | Nothing | Array of strings. The name of a property this is an alias for. See below.                                                                 |                                                                             |
| `longhands`                | No       | `[]`    | Array of strings. If this is a shorthand, these are the property names that it expands out into.                                          | `Vector<PropertyID> longhands_for_shorthand(PropertyID)`                    |
| `max-values`               | No       | `1`     | Integer. How many values can be parsed for this property. eg, `margin` can have up to 4 values.                                           | `size_t property_maximum_value_count(PropertyID)`                           |
| `percentages-resolve-to`   | No       | Nothing | String. What type percentages get resolved to. eg, for `width` percentages are resolved to `length` values.                               | `Optional<ValueType> property_resolves_percentages_relative_to(PropertyID)` |
| `quirks`                   | No       | `[]`    | Array of strings. Some properties have special behavior in "quirks mode", which are listed here. See below.                               | `bool property_has_quirk(PropertyID, Quirk)`                                |
| `valid-identifiers`        | No       | `[]`    | Array of strings. Which keywords the property accepts. Consider defining an enum instead and putting its name in the `valid-types` array. | `bool property_accepts_keyword(PropertyID, Keyword)`                        |
| `valid-types`              | No       | `[]`    | Array of strings. Which value types the property accepts. See below.                                                                      | `bool property_accepts_type(PropertyID, ValueType)`                         |

### `animation-type`

The [Web Animations spec](https://www.w3.org/TR/web-animations/#animation-type) defines the valid values here:

| Spec term         | JSON value          |
|-------------------|---------------------|
| not animatable    | `none`              |
| discrete          | `discrete`          |
| by computed value | `by-computed-value` |
| repeatable list   | `repeatable-list`   |
| (See prose)       | `custom`            |

### `legacy-alias-for` and `logical-alias-for`

These are two separate concepts, with unfortunately similar names:
- [Legacy name aliases](https://drafts.csswg.org/css-cascade-5/#legacy-name-alias) are properties whose spec names have changed,
  but the syntax has not, so setting the old one is defined as setting the new one directly.
  For example, `font-stretch` was renamed to `font-width`, so `font-stretch` is now a legacy name alias for `font-width`.
- Logical aliases are properties like `margin-block-start`, which may assign a value to one of several other properties
  (`margin-top`, `margin-bottom`, `margin-left`, or `margin-right`) depending on the element they are applied to.
  List all the properties that they can alias.

### `quirks`

The [Quirks spec](https://quirks.spec.whatwg.org/#css) defines these.

| Spec term                    | JSON value           |
|------------------------------|----------------------|
| The hashless hex color quirk | `hashless-hex-color` |
| The unitless length quirk    | `unitless-length`    |

### `valid-types`

The `valid-types` array lists the names of CSS value types, as defined in the latest
[CSS Values and Units spec](https://www.w3.org/TR/css-values/), without the `<>` around them.
For numeric types, we use the [bracketed range notation](https://www.w3.org/TR/css-values-4/#css-bracketed-range-notation),
for example `width` can take any non-negative length, so it has `"length [0,∞]"` in its `valid-types` array.
For `<custom-ident>`s, the excluded identifiers are placed within `![]`, for example `"custom-ident ![all,none]"`.

## Keywords.json

This is a single JSON array of strings, each of which is a CSS keyword, for example `auto`, `none`, `medium`, or `currentcolor`.
This generates `Keyword.h` and `Keyword.cpp`.
All keyword values used by any property or media-feature need to be defined here.

The generated code provides:
- A `Keyword` enum as used by `CSSKeywordValue`
- `Optional<Keyword> keyword_from_string(StringView)` to attempt to convert a string into a Keyword
- `StringView string_from_keyword(Keyword)` to convert a Keyword back into a string
- `bool is_css_wide_keyword(StringView)` which returns whether the string is one of the special "CSS-wide keywords"

## Enums.json

This is a single JSON object, with enum names as keys and the values being arrays of keyword names.
This generates `Enums.h` and `Enums.cpp`.

We often want to define an enum that's a set of a few keywords.
`Enums.json` allows you to generate these enums automatically, along with functions to convert them to and from a Keyword,
or convert them to a string.
These enums also can be used in property definitions in `Properties.json` by putting their name in the `valid-types` array.
This helps reduce repetition, for example the `border-*-style` properties all accept the same set of keywords, so they
are implemented as a `line-style` enum.

The generated code provides these for each enum, using "foo" as an example:
- A `Foo` enum for its values
- `Optional<Foo> keyword_to_foo(Keyword)` to convert a `Keyword` to a `Foo`
- `Keyword to_keyword(Foo)` to convert the `Foo` back to a `Keyword`
- `StringView to_string(Foo)` to convert the `Foo` directly to a string

## PseudoClasses.json

This is a single JSON object, with selector pseudo-class names as keys and the values being objects with fields for the pseudo-class.
This generates `PseudoClass.h` and `PseudoClass.cpp`.

Each entry has a single required property, `argument`, which is a string containing the grammar for the pseudo-class's
function parameters - for identifier-style pseudo-classes it is left blank.
The grammar is taken directly from the spec.

The generated code provides:
- A `PseudoClass` enum listing every pseudo-class name
- `Optional<PseudoClass> pseudo_class_from_string(StringView)` to parse a string as a `PseudoClass` name
- `StringView pseudo_class_name(PseudoClass)` to convert a `PseudoClass` back into a string
- The `PseudoClassMetadata` struct which holds a representation of the data from the JSON file
- `PseudoClassMetadata pseudo_class_metadata(PseudoClass)` to retrieve that data

## PsuedoElements.json

This is a single JSON object, with pseudo-element names as keys and the values being objects with fields for the pseudo-element.
This generated `PsuedoElement.h` and `PseudoElement.cpp`.

Each entry has the following properties:

| Field                | Required | Default        | Description                                                                                                                                                            |
|----------------------|----------|----------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `alias-for`          | No       | Nothing        | Use to specify that this should be treated as an alias for the named pseudo-element.                                                                                   |
| `function-syntax`    | No       | Nothing        | Syntax for the function arguments if this is a function-type pseudo-element. Copied directly from the spec.                                                            |
| `is-generated`       | No       | `false`        | Whether this is a [generated pseudo-element.](https://drafts.csswg.org/css-pseudo-4/#generated-content)                                                                |
| `is-allowed-in-has`  | No       | `false`        | Whether this is a [`:has`-allowed pseudo-element.](https://drafts.csswg.org/selectors/#has-allowed-pseudo-element)                                                     |
| `property-whitelist` | No       | Nothing        | Some pseudo-elements only permit certain properties. If so, name them in an array here. Some special values are allowed here for categories of properties - see below. |
| `spec`               | No       | Nothing        | Link to the spec definition, for reference. Not used in generated code.                                                                                                |
| `type`               | No       | `"identifier"` | What type of pseudo-element is this. Either "identifier", "function", or "both".                                                                                       |

The generated code provides:
- A `PseudoElement` enum listing every pseudo-element name
- `Optional<PseudoElement> pseudo_element_from_string(StringView)` to parse a string as a `PseudoElement` name
- `Optional<PseudoElement> aliased_pseudo_element_from_string(StringView)` is similar, but returns the `PseudoElement` this name is an alias for
- `StringView pseudo_element_name(PseudoElement)` to convert a `PseudoElement` back into a string
- `bool is_has_allowed_pseudo_element(PseudoElement)` returns whether the pseudo-element is valid inside `:has()`
- `bool pseudo_element_supports_property(PseudoElement, PropertyID)` returns whether the property can be applied to this pseudo-element
- A `GeneratedPseudoElement` enum listing only the pseudo-elements that are [generated content](https://drafts.csswg.org/css-pseudo-4/#generated-content)
- `Optional<GeneratedPseudoElement> to_generated_pseudo_element(PseudoElement)` for converting from `PseudoElement` to `GeneratedPseudoElement`. Returns nothing if it's not a generated pseudo-element
- `PseudoElement to_pseudo_element(GeneratedPseudoElement)` does the opposite conversion

### `property-whitelist`

This is an array of strings. Properties can be named directly ("color"), or categories of properties with a leading `#`
("#font-properties"), as the specs often says a group is allowed instead of listing the properties exactly.
Any properties we don't support yet can be prefixed with "FIXME:" and will be ignored.

The following categories are supported:

- `#background-properties`: `background` and its longhands
- `#border-properties`: `border`, `border-radius`, and their longhands
- `#custom-properties`: Custom properties, AKA CSS variables
- `#font-properties`: `font`, its longhands, and other `font-*` properties
- `#inline-layout-properties`: Properties defined in [CSS Inline](https://drafts.csswg.org/css-inline-3/)
- `#inline-typesetting-properties`: Properties defined in [CSS Text](https://drafts.csswg.org/css-text-4/)
- `#margin-properties`: `margin` and its longhands
- `#padding-properties`: `padding` and its longhands
- `#text-decoration-properties`: `text-decoration` and its longhands

## MediaFeatures.json

This is a single JSON object, with media-feature names as keys and the values being objects with fields for the media-feature.
This generates `MediaFeatureID.h` and `MediaFeatureID.cpp`.

A `<media-feature>` is a value that a media query can inspect.
They are listed in the [`@media` descriptor table](https://www.w3.org/TR/mediaqueries-5/#media-descriptor-table) in the latest Media Queries spec.

The definitions here are like a simplified version of the `Properties.json` definitions.

| Field    | Description                                                                                                                                                                                       |
|----------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `type`   | String. How the media-feature is evaluated, either `discrete` or `range`.                                                                                                                         |
| `values` | Array of strings. These are directly taken from the spec, with keywords as they are, and `<>` around type names. Types may be `<boolean>`, `<integer>`, `<length>`, `<ratio>`, or `<resolution>`. |

The generated code provides:
- A `MediaFeatureValueType` enum listing the possible value types
- A `MediaFeatureID` enum, listing each media-feature
- `Optional<MediaFeatureID> media_feature_id_from_string(StringView)` to convert a string to a `MediaFeatureID`
- `StringView string_from_media_feature_id(MediaFeatureID)` to convert a `MediaFeatureID` back to a string
- `bool media_feature_type_is_range(MediaFeatureID)` returns whether the media feature is a `range` type, as opposed to a `discrete` type
- `bool media_feature_accepts_type(MediaFeatureID, MediaFeatureValueType)` returns whether the media feature will accept values of this type
- `bool media_feature_accepts_keyword(MediaFeatureID, Keyword)` returns whether the media feature accepts this keyword

## MathFunctions.json

This is a single JSON object, describing each [CSS math function](https://www.w3.org/TR/css-values/#math-function),
with the keys being the function name and the values being objects describing that function's properties.
This generates `MathFunctions.h` and `MathFunctions.cpp`.

Each entry has two properties:

| Field                  | Description                                                                                                                                                                                                            |
|------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `parameter-validation` | Optional string. Either "same" or "consistent", depending on whether the spec says the input calculations should be the same type or consistent types. Defaults to "same". Ignore this if there is only one parameter. |
| `parameters`           | An array of parameter definition objects, see below.                                                                                                                                                                   |

Parameter definitions have the following properties:

| Field      | Description                                                                      |
|------------|----------------------------------------------------------------------------------|
| `name`     | String. Name of the parameter, as given in the spec.                             |
| `type`     | String. Accepted types for the parameter, as a single string, separated by `\|`. |
| `required` | Boolean. Whether this parameter is required.                                     |

The generated code provides:
- A `MathFunction` enum listing the math functions
- The implementation of the CSS Parser's `parse_math_function()` method

## TransformFunctions.json

This is a single JSON object, describing each [CSS transform function](https://www.w3.org/TR/css-transforms/#transform-functions),
with the keys being the function name and the values being objects describing that function's properties.
This generates `TransformFunctions.h` and `TransformFunctions.cpp`.

Each entry currently has a single property, `parameters`, which is an array of parameter definition objects.
Parameter definitions have the following properties:

| Field      | Description                                  |
|------------|----------------------------------------------|
| `type`     | String. Accepted type for the parameter.     |
| `required` | Boolean. Whether this parameter is required. |

The generated code provides:
- A `TransformFunction` enum listing the transform functions
- `Optional<TransformFunction> transform_function_from_string(StringView)` to parse a string as a `TransformFunction`
- `StringView to_string(TransformFunction)` to convert a `TransformFunction` back to a string
- `TransformFunctionMetadata transform_function_metadata(TransformFunction)` to obtain metadata about the transform function, such as its parameter list
