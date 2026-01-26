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

| Field                               | Required | Default    | Description                                                                                                 | Generated functions                                                                                                                                                                                                    |
|-------------------------------------|----------|------------|-------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `affects-layout`                    | No       | `true`     | Boolean. Whether changing this property will invalidate the element's layout.                               | `bool property_affects_layout(PropertyID)`                                                                                                                                                                             |
| `affects-stacking-context`          | No       | `false`    | Boolean. Whether this property can cause a new stacking context for the element.                            | `bool property_affects_stacking_context(PropertyID)`                                                                                                                                                                   |
| `animation-type`                    | Yes      |            | String. How the property should be animated. Defined by the spec. See below.                                | `AnimationType animation_type_from_longhand_property(PropertyID)`                                                                                                                                                      |
| `inherited`                         | Yes      |            | Boolean. Whether the property is inherited by its child elements. Only applicable to longhand properties.   | `bool is_inherited_property(PropertyID)`                                                                                                                                                                               |
| `initial`                           | Yes      |            | String. The property's initial value if it is not specified.                                                | `NonnullRefPtr<StyleValue const> property_initial_value(PropertyID)`                                                                                                                                                   |
| `legacy-alias-for`                  | No       | Nothing    | String. The name of a property this is an alias for. See below.                                             |                                                                                                                                                                                                                        |
| `logical-alias-for`                 | No       | Nothing    | An object. See below.                                                                                       | `bool property_is_logical_alias(PropertyID);`<br/>`PropertyID map_logical_alias_to_physical_property(PropertyID, LogicalAliasMappingContext const&)`                                                                   |
| `longhands`                         | No       | `[]`       | Array of strings. If this is a shorthand, these are the property names that it expands out into.            | `Vector<PropertyID> longhands_for_shorthand(PropertyID)`<br/>`Vector<PropertyID> expanded_longhands_for_shorthand(PropertyID)`<br/>`Vector<PropertyID> shorthands_for_longhand(PropertyID)`                            |
| `max-values`                        | No       | `1`        | Integer. How many values can be parsed for this property. eg, `margin` can have up to 4 values.             | `size_t property_maximum_value_count(PropertyID)`                                                                                                                                                                      |
| `multiplicity`                      | No       | `"single"` | String. Category for whether this property is a single value or a list of values. See below.                | `bool property_is_single_valued(PropertyID)`<br/>`bool property_is_list_valued(PropertyID)`<br/>`PropertyMultiplicity property_multiplicity(PropertyID)`                                                               |
| `percentages-resolve-to`            | No       | Nothing    | String. What type percentages get resolved to. eg, for `width` percentages are resolved to `length` values. | `Optional<ValueType> property_resolves_percentages_relative_to(PropertyID)`                                                                                                                                            |
| `positional-value-list-shorthand`   | No       | `false`    | Boolean. Whether this property is a "positional value list shorthand". See below.                           | `bool property_is_positional_value_list_shorthand(PropertyID)`                                                                                                                                                         |
| `quirks`                            | No       | `[]`       | Array of strings. Some properties have special behavior in "quirks mode", which are listed here. See below. | `bool property_has_quirk(PropertyID, Quirk)`                                                                                                                                                                           |
| `requires-computation`              | Yes      |            | String. When a property's value needs to be run through the computation process. See below.                 | `bool property_requires_computation_with_inherited_value(PropertyID)`<br/>`bool property_requires_computation_with_initial_value(PropertyID)`<br/>`bool property_requires_computation_with_cascaded_value(PropertyID)` |
| `valid-identifiers`                 | No       | `[]`       | Array of strings. Which keywords the property accepts. See below.                                           | `bool property_accepts_keyword(PropertyID, Keyword)`<br/>`Optional<Keyword> resolve_legacy_value_alias(PropertyID, Keyword)`                                                                                           |
| `valid-types`                       | No       | `[]`       | Array of strings. Which value types the property accepts. See below.                                        | `bool property_accepts_type(PropertyID, ValueType)`                                                                                                                                                                    |
| `needs-layout-for-getcomputedstyle` | No       | `false`    | Boolean. Whether this property requires up-to-date layout before it could be queried by getComputedStyle()  | `bool property_needs_layout_for_getcomputedstyle(PropertyID)`                                                                                                                                                          |

### `animation-type`

The [Web Animations spec](https://www.w3.org/TR/web-animations/#animation-type) defines the valid values here:

| Spec term         | JSON value          |
|-------------------|---------------------|
| not animatable    | `none`              |
| discrete          | `discrete`          |
| by computed value | `by-computed-value` |
| repeatable list   | `repeatable-list`   |
| (See prose)       | `custom`            |

### `legacy-alias-for`
(Not to be confused with `logical-alias-for` below.)

[Legacy name aliases](https://drafts.csswg.org/css-cascade-5/#legacy-name-alias) are properties whose spec names have changed,
but the syntax has not, so setting the old one is defined as setting the new one directly.
For example, `font-stretch` was renamed to `font-width`, so `font-stretch` is now a legacy name alias for `font-width`.

### `logical-alias-for`
(Not to be confused with `legacy-alias-for` above.)

Logical aliases are properties like `margin-block-start`, which may assign a value to one of several other properties
(`margin-top`, `margin-bottom`, `margin-left`, or `margin-right`) depending on the element they are applied to.

`logical-alias-for` should be an object with two fields, both of which are required:

| Field     | Description                                                                                                                         |
|-----------|-------------------------------------------------------------------------------------------------------------------------------------|
| `group`   | String. Name of the logical property group this is associated with. (See [LogicalPropertyGroups.json](#logicalpropertygroupsjson).) |
| `mapping` | String. How this relates to the group. eg, if it's the block end value, `block-end`.                                                |

### `multiplicity`
The three possible values are `"single"`, `"list"`, and `"coordinating-list"`.

Most properties represent a single "thing", possibly made of multiple parts. For example, `border` takes a color, style
and thickness, but it's still a single border. Others are a list: `background` takes multiple layers, separated by
commas; `counter-increment` doesn't have commas but also is a list of counters that are incremented.

Lists can also be [coordinating list properties](https://drafts.csswg.org/css-values-4/#linked-properties), in which
case they are marked as `coordinating-list` instead of `list`.

### `positional-value-list-shorthand`
Some shorthand properties work differently to normal in that mapping of provided values to longhands isn't necessarily
1-to-1 and instead depends on the number of values provided, for example `margin`, `border-width`, `gap`, etc.

These properties have distinct behaviors in how they are parsed and serialized, having them marked allows us to
implement this behavior in a generic way.

### `quirks`

The [Quirks spec](https://quirks.spec.whatwg.org/#css) defines these.

| Spec term                    | JSON value           |
|------------------------------|----------------------|
| The hashless hex color quirk | `hashless-hex-color` |
| The unitless length quirk    | `unitless-length`    |

### `requires-computation`

There are three ways the specified value of a property on an element can be determined:

- Inherited, e.g. it is an inherited property and no value has been specified, or the `inherit` keyword has been
  specified
- Initial, e.g. it is _not_ an inherited property and no value has been specified, or the `initial` keyword has been
  specified
- Cascaded, a value has been cascaded for this element based on the styles

We then turn that specified value into a computed value by running the computation (see
`StyleComputer::compute_value_of_property`).

Depending on the property, we know that for some of the above cases the computed value is guaranteed to be the same as
the specified value, by specifying the cases where it _isn't_ we can limit running the computation process to only when
required.

| Value                 | Behavior                                                                                                      |
| --------------------- | ------------------------------------------------------------------------------------------------------------- |
| `always`              | Always runs the computation process, regardless of how the specified value was determined                     |
| `non-inherited-value` | Only run the computation process if the specified value was determined based on the initial or cascaded value |
| `cascaded-value`      | Only run the computation process if the specified value was determined based on a cascaded value              |
| `never`               | Always skip the computation process                                                                           |

### `valid-identifiers`

A list of CSS keyword names, that the property accepts. Consider defining an enum instead and putting its name in the
`valid-types` array, especially if the spec provides a name to a set of such keywords.

Some properties have [legacy value aliases](https://drafts.csswg.org/css-cascade-5/#css-legacy-value-alias), where one
keyword is parsed as another. These are supported as `"foo>bar"`, to make `foo` an alias for `bar`.

### `valid-types`

The `valid-types` array lists the names of CSS value types, as defined in the latest
[CSS Values and Units spec](https://www.w3.org/TR/css-values/), without the `<>` around them.
For numeric types, we use the [bracketed range notation](https://www.w3.org/TR/css-values-4/#css-bracketed-range-notation),
for example `width` can take any non-negative length, so it has `"length [0,∞]"` in its `valid-types` array.
For `<custom-ident>`s, the excluded identifiers are placed within `![]`, for example `"custom-ident ![all,none]"`.

## LogicalPropertyGroups.json

A set of matching CSS properties can be grouped together in what's called a
["logical property group"](https://drafts.csswg.org/css-logical-1/#logical-property-group).
For example, all of the `margin-*` properties are in the `margin` group.

This data is used to map logical properties (such as `margin-block-start`) into their physical counterparts (like
`margin-top`) at runtime, depending on the writing direction. We don't generate any code directly from this file.
Instead, it's used as part of generating the PropertyID code.

The file is a single object where the keys are the names of the logical property groups, and their values are objects
mapping physical dimensions, sides, or corners to the relevant property. Which keys are there depends on the group - for
example the `size` group has `width` and `height`.

## Descriptors.json

Descriptors are basically properties, but for at-rules instead of style. The overall structure is a JSON object, with
keys being at-rule names and the values being data about those at-rules. The main part is the data about the descriptors
that the at-rule can have.

The generated code provides:
- An `AtRuleID` enum, mostly used as a parameter for parsing descriptors, as multiple at-rules may have descriptors with 
  the same name.
- `FlyString to_string(AtRuleID)`, mostly for debug logging.
- A `DescriptorID` enum, listing every descriptor.
- `Optional<DescriptorID> descriptor_id_from_string(AtRuleID, StringView)` for getting a DescriptorID from a string, if
  it exists in that at-rule.
- `FlyString to_string(DescriptorID)` for serializing descriptor names.
- `bool at_rule_supports_descriptor(AtRuleID, DescriptorID)` to query if the given at-rule allows the descriptor.
- `RefPtr<StyleValue const> descriptor_initial_value(AtRuleID, DescriptorID)` for getting a descriptor's initial value.
- `DescriptorMetadata get_descriptor_metadata(AtRuleID, DescriptorID)` returns data used for parsing the descriptor.

### At-rule fields

Each at-rule object has the following fields. Both are required.

| Field         | Description                                                                                       |
|---------------|---------------------------------------------------------------------------------------------------|
| `spec`        | String. URL to the spec that defines this at-rule.                                                |
| `descriptors` | Object, with keys being descriptor names and values being objects of their properties. See below. |

### Descriptor fields

Each descriptor object can have the following fields:

| Field              | Required | Description                                                           |
|--------------------|----------|-----------------------------------------------------------------------|
| `initial`          | No       | String. The descriptor's initial value if none is provided.           |
| `legacy-alias-for` | No       | String. The name of a different descriptor that this is an alias for. |
| `syntax`           | Yes      | Array of strings. Each string is one option, taken from the spec.     |
| `FIXME` or `NOTE`  | No       | Strings, for when you want to leave a note.                           |

## Keywords.json

This is a single JSON array of strings, each of which is a CSS keyword, for example `auto`, `none`, `medium`, or `currentcolor`.
This generates `Keyword.h` and `Keyword.cpp`.
All keyword values used by any property or media-feature need to be defined here.

The generated code provides:
- A `Keyword` enum as used by `KeywordStyleValue`
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

Each entry has the following properties:

| Field                | Required                               | Default        | Description                                                                                                                                                                     |
|----------------------|----------------------------------------|----------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `argument`           | Unless `legacy-alias-for` is specified | Nothing        | A string containing the grammar for the pseudo-class's function parameters - for identifier-style pseudo-classes it is left blank. The grammar is taken directly from the spec. |
| `legacy-alias-for`   | No                                     | Nothing        | Use to specify that this should be treated as a [legacy selector alias](https://drafts.csswg.org/selectors/#legacy-selector-alias) for the named pseudo-class.                  |

The generated code provides:
- A `PseudoClass` enum listing every pseudo-class name
- `Optional<PseudoClass> pseudo_class_from_string(StringView)` to parse a string as a `PseudoClass` name
- `StringView pseudo_class_name(PseudoClass)` to convert a `PseudoClass` back into a string
- The `PseudoClassMetadata` struct which holds a representation of the data from the JSON file
- `PseudoClassMetadata pseudo_class_metadata(PseudoClass)` to retrieve that data

## PseudoElements.json

This is a single JSON object, with pseudo-element names as keys and the values being objects with fields for the pseudo-element.
This generated `PseudoElement.h` and `PseudoElement.cpp`.

Each entry has the following properties:

| Field                | Required | Default        | Description                                                                                                                                                            |
|----------------------|----------|----------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `alias-for`          | No       | Nothing        | Use to specify that this should be treated as an alias for the named pseudo-element.                                                                                   |
| `function-syntax`    | No       | Nothing        | Syntax for the function arguments if this is a function-type pseudo-element. Copied directly from the spec.                                                            |
| `is-allowed-in-has`  | No       | `false`        | Whether this is a [`:has`-allowed pseudo-element](https://drafts.csswg.org/selectors/#has-allowed-pseudo-element).                                                     |
| `is-pseudo-root`     | No       | `false`        | Whether this is a [pseudo-element root](https://drafts.csswg.org/css-view-transitions/#pseudo-element-root).                                                           |
| `property-whitelist` | No       | Nothing        | Some pseudo-elements only permit certain properties. If so, name them in an array here. Some special values are allowed here for categories of properties - see below. |
| `spec`               | No       | Nothing        | Link to the spec definition, for reference. Not used in generated code.                                                                                                |
| `type`               | No       | `"identifier"` | What type of pseudo-element is this. Either "identifier", "function", or "both".                                                                                       |

The generated code provides:
- A `PseudoElement` enum listing every pseudo-element name
- `Optional<PseudoElement> pseudo_element_from_string(StringView)` to parse a string as a `PseudoElement` name
- `Optional<PseudoElement> aliased_pseudo_element_from_string(StringView)` is similar, but returns the `PseudoElement` this name is an alias for
- `StringView pseudo_element_name(PseudoElement)` to convert a `PseudoElement` back into a string
- `bool is_has_allowed_pseudo_element(PseudoElement)` returns whether the pseudo-element is valid inside `:has()`
- `bool is_pseudo_element_root(PseudoElement)` returns whether the pseudo-element is a [pseudo-element root](https://drafts.csswg.org/css-view-transitions/#pseudo-element-root)
- `bool pseudo_element_supports_property(PseudoElement, PropertyID)` returns whether the property can be applied to this pseudo-element

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

| Field            | Description                                                                                                                                                                                       |
|------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `type`           | String. How the media-feature is evaluated, either `discrete` or `range`.                                                                                                                         |
| `values`         | Array of strings. These are directly taken from the spec, with keywords as they are, and `<>` around type names. Types may be `<boolean>`, `<integer>`, `<length>`, `<ratio>`, or `<resolution>`. |
| `false-keywords` | Array of strings. These are any keywords that should be considered false when the media feature is evaluated as `@media (foo)`. Generally this will be a single value, such as `"none"`.          |

The generated code provides:
- A `MediaFeatureValueType` enum listing the possible value types
- A `MediaFeatureID` enum, listing each media-feature
- `Optional<MediaFeatureID> media_feature_id_from_string(StringView)` to convert a string to a `MediaFeatureID`
- `StringView string_from_media_feature_id(MediaFeatureID)` to convert a `MediaFeatureID` back to a string
- `bool media_feature_type_is_range(MediaFeatureID)` returns whether the media feature is a `range` type, as opposed to a `discrete` type
- `bool media_feature_accepts_type(MediaFeatureID, MediaFeatureValueType)` returns whether the media feature will accept values of this type
- `bool media_feature_accepts_keyword(MediaFeatureID, Keyword)` returns whether the media feature accepts this keyword
- `bool media_feature_keyword_is_falsey(MediaFeatureID, Keyword)` returns whether the given keyword is considered false when the media-feature is evaluated in a boolean context. (Like `@media (foo)`)

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

## EnvironmentVariables.json

This is a single JSON object, describing each [CSS environment variable](https://drafts.csswg.org/css-env/#css-environment-variable),
with the keys being the environment variable names, and the values being objects describing the variable's properties.
This generates `EnvironmentVariable.h` and `EnvironmentVariable.cpp`.

Each entry has 3 properties, all taken from the spec:

| Field        | Description                                                         |
|--------------|---------------------------------------------------------------------|
| `spec`       | String. URL to the spec definition for this environment variable.   |
| `type`       | String. CSS value type of the variable, eg `<length>`.              |
| `dimensions` | Integer. Number of dimensions for the variable, or `0` for scalars. |

The generated code provides:
- An `EnvironmentVariable` enum listing the environment variables
- `Optional<EnvironmentVariable> environment_variable_from_string(StringView)` to parse a string as an `EnvironmentVariable`
- `StringView to_string(EnvironmentVariable)` to convert the `EnvironmentVariable` back to a string
- `ValueType environment_variable_type(EnvironmentVariable)` to get the variable's value type
- `u32 environment_variable_dimension_count(EnvironmentVariable)` to get its dimension count

## Units.json

This is a JSON object with the keys being dimension type names, and the values being objects. Those objects' keys are
unit names, and their values are data about each unit.
It generates `Units.h` and `Units.cpp`.

Each unit has the following properties:

| Field                      | Description                                                                                                                        |
|----------------------------|------------------------------------------------------------------------------------------------------------------------------------|
| `is-canonical-unit`        | Boolean, default `false`. Each dimension has one canonical unit.                                                                   |
| `number-of-canonical-unit` | Number. How many of the canonical units 1 of this is equivalent to. Ignore this for relative units, and the canonical unit itself. |
| `is-relative-to`           | String. Some length units are relative to the font or viewport. Set this to `"font"` or `"viewport"` for those.                    |

The generated code provides:
- A `DimensionType` enum, listing each type of dimension that has units defined.
- `Optional<DimensionType> dimension_for_unit(StringView)` for querying which dimension a unit applies to, if any.
- A `FooUnit` enum for each dimension "foo", which lists all the units of that dimension.
- For each of those...
  - `constexpr FooUnit canonical_foo_unit()` which is the canonical unit for that type.
  - `Optional<FooUnit> string_to_foo_unit(StringView)` for parsing a unit from a string.
  - `StringView to_string(FooUnit)` for serializing those units.
  - `bool units_are_compatible(FooUnit, FooUnit)` which returns whether these are compatible - basically whether you can convert from one to the other.
  - `double ratio_between_units(FooUnit, FooUnit)` to get a multiplier for converting the first unit into the second.
- `bool is_absolute(LengthUnit)`, `bool is_font_relative(LengthUnit)`, `bool is_viewport_relative(LengthUnit)`, and `bool is_relative(LengthUnit)` for checking the category of length units.
