/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::env;
use std::error::Error;
use std::fmt::Write;
use std::path::{Path, PathBuf};

// Mirrors title_casify() in Meta/Utils/utils.py.
fn title_casify(dashy_name: &str) -> String {
    dashy_name
        .split('-')
        .filter(|part| !part.is_empty())
        .map(|part| {
            let mut chars = part.chars();
            let first = chars.next().unwrap().to_ascii_uppercase();
            format!("{first}{}", chars.as_str())
        })
        .collect()
}

fn ordered_pseudo_element_names(
    pseudo_elements: &serde_json::Map<String, serde_json::Value>,
) -> Result<Vec<String>, Box<dyn Error>> {
    let mut synthetic = Vec::new();
    let mut element_reference = Vec::new();
    let mut functional = Vec::new();
    for (name, value) in pseudo_elements {
        let object = value.as_object().unwrap();
        if object.contains_key("alias-for") {
            continue;
        }
        if object.get("type").and_then(|value| value.as_str()) == Some("function") {
            functional.push(name.clone());
            continue;
        }
        match object.get("implementation").and_then(|value| value.as_str()) {
            Some("synthetic") => synthetic.push(name.clone()),
            Some("element-reference") => element_reference.push(name.clone()),
            other => return Err(format!("invalid or missing implementation type for ::{name}: {other:?}").into()),
        }
    }
    synthetic.extend(element_reference);
    synthetic.extend(functional);
    Ok(synthetic)
}

fn load_property_groups(path: &Path) -> Result<std::collections::HashMap<String, Vec<String>>, Box<dyn Error>> {
    let mut groups = std::collections::HashMap::new();
    let mut current_group = None;
    for raw_line in std::fs::read_to_string(path)?.lines() {
        let line = raw_line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if let Some(group) = line.strip_prefix('[').and_then(|line| line.strip_suffix(']')) {
            groups.insert(format!("#{group}"), Vec::new());
            current_group = Some(format!("#{group}"));
            continue;
        }
        let Some(group) = current_group.as_ref() else {
            return Err(format!("property outside a pseudo-element property group: {line}").into());
        };
        groups.get_mut(group).unwrap().push(line.to_string());
    }
    Ok(groups)
}

fn write_enum_and_from_ffi(output: &mut String, enum_name: &str, variants: &[String]) {
    writeln!(output, "#[derive(Clone, Copy, Debug, PartialEq, Eq)]").unwrap();
    writeln!(output, "#[repr(u8)]").unwrap();
    writeln!(output, "pub enum {enum_name} {{").unwrap();
    for variant in variants {
        writeln!(output, "    {variant},").unwrap();
    }
    writeln!(output, "}}").unwrap();
    writeln!(output).unwrap();
    let fn_name = if enum_name == "PseudoClassType" {
        "pseudo_class_from_ffi"
    } else {
        "pseudo_element_from_ffi"
    };
    writeln!(output, "fn {fn_name}(value: u8) -> {enum_name} {{").unwrap();
    writeln!(output, "    match value {{").unwrap();
    for (index, variant) in variants.iter().enumerate() {
        writeln!(output, "        {index} => {enum_name}::{variant},").unwrap();
    }
    writeln!(output, "        _ => panic!(\"invalid {enum_name} {{value}}\"),").unwrap();
    writeln!(output, "    }}").unwrap();
    writeln!(output, "}}").unwrap();
    writeln!(output).unwrap();
}

// Generates the Rust twins of the C++ PseudoClass and PseudoElement enums from the same JSON
// sources as Meta/Generators/generate_libweb_css_pseudo_{class,element}.py, so the numeric
// values that cross the selector FFI cannot drift out of sync with the C++ side.
fn generate_selector_pseudo_types(manifest_dir: &Path, out_dir: &Path) -> Result<(), Box<dyn Error>> {
    let css_dir = manifest_dir.parent().unwrap().join("CSS");
    let pseudo_classes_path = css_dir.join("PseudoClasses.json");
    let pseudo_elements_path = css_dir.join("PseudoElements.json");
    println!("cargo:rerun-if-changed={}", pseudo_classes_path.display());
    println!("cargo:rerun-if-changed={}", pseudo_elements_path.display());

    let parse_object = |path: &Path| -> Result<serde_json::Map<String, serde_json::Value>, Box<dyn Error>> {
        let value: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(path)?)?;
        match value {
            serde_json::Value::Object(object) => Ok(object),
            _ => Err(format!("{} does not contain a JSON object", path.display()).into()),
        }
    };

    let pseudo_class_names = parse_object(&pseudo_classes_path)?
        .iter()
        .filter(|(_, value)| !value.as_object().unwrap().contains_key("legacy-alias-for"))
        .map(|(name, _)| title_casify(name))
        .collect::<Vec<_>>();

    let pseudo_elements = parse_object(&pseudo_elements_path)?;
    let mut pseudo_element_names = ordered_pseudo_element_names(&pseudo_elements)?
        .iter()
        .map(|name| title_casify(name))
        .collect::<Vec<_>>();
    // NB: The C++ generator emits UnknownWebKit after KnownPseudoElementCount, so its FFI value is
    //     the number of known pseudo-elements.
    pseudo_element_names.push("UnknownWebKit".to_string());

    let mut output = String::new();
    writeln!(
        output,
        "// Generated by build.rs from CSS/PseudoClasses.json and CSS/PseudoElements.json."
    )
    .unwrap();
    writeln!(output).unwrap();
    write_enum_and_from_ffi(&mut output, "PseudoClassType", &pseudo_class_names);
    write_enum_and_from_ffi(&mut output, "PseudoElementType", &pseudo_element_names);

    std::fs::write(out_dir.join("selector_pseudo_generated.rs"), output)?;
    Ok(())
}

// Generates the property metadata tables for the Rust style computation core from
// Properties.json, replicating the C++ generator's identifier assignment exactly:
// Custom is 0, then shorthands, inherited longhands and non-inherited longhands in
// JSON order, skipping legacy aliases. A C++ unit test compares the tables against
// the C++-generated equivalents so the two generators cannot drift.
fn generate_property_metadata(manifest_dir: &Path, out_dir: &Path) -> Result<(), Box<dyn Error>> {
    let properties_path = manifest_dir.parent().unwrap().join("CSS/Properties.json");
    println!("cargo:rerun-if-changed={}", properties_path.display());

    let value: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&properties_path)?)?;
    let serde_json::Value::Object(properties) = value else {
        return Err("Properties.json does not contain a JSON object".into());
    };

    // Logical alias properties take their metadata from the first physical property of
    // their logical group, with local overrides, mirroring the C++ generator.
    let groups_path = manifest_dir.parent().unwrap().join("CSS/LogicalPropertyGroups.json");
    println!("cargo:rerun-if-changed={}", groups_path.display());
    let groups_value: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&groups_path)?)?;
    let serde_json::Value::Object(groups) = groups_value else {
        return Err("LogicalPropertyGroups.json does not contain a JSON object".into());
    };
    let first_physical_in_group =
        |group_name: &str| -> Option<&str> { groups[group_name]["physical"].as_object()?.values().next()?.as_str() };
    let property_field = |name: &str, field: &str| -> Option<serde_json::Value> {
        let object = properties[name].as_object().unwrap();
        if let Some(value) = object.get(field) {
            return Some(value.clone());
        }
        let group = object.get("logical-alias-for")?.as_object()?.get("group")?.as_str()?;
        let physical = first_physical_in_group(group)?;
        properties[physical].as_object().unwrap().get(field).cloned()
    };

    let mut shorthands = Vec::new();
    let mut inherited_longhands = Vec::new();
    let mut noninherited_longhands = Vec::new();
    for (name, value) in &properties {
        let object = value.as_object().unwrap();
        if object.contains_key("legacy-alias-for") {
            continue;
        }
        // NB: The C++ generator populates the "all" shorthand's longhand list
        //     programmatically, so it has no explicit "longhands" key.
        if object.contains_key("longhands") || name == "all" {
            shorthands.push(name.clone());
        } else if property_field(name, "inherited").unwrap().as_bool().unwrap() {
            inherited_longhands.push(name.clone());
        } else {
            noninherited_longhands.push(name.clone());
        }
    }

    // Identifier assignment: Custom = 0, then the three runs in order.
    let mut ids = std::collections::HashMap::new();
    for (index, name) in shorthands
        .iter()
        .chain(&inherited_longhands)
        .chain(&noninherited_longhands)
        .enumerate()
    {
        ids.insert(name.clone(), (index + 1) as u16);
    }
    let first_longhand = ids[&inherited_longhands[0]];
    let last_longhand = ids[noninherited_longhands.last().unwrap()];
    let first_inherited = ids[&inherited_longhands[0]];
    let last_inherited = ids[inherited_longhands.last().unwrap()];

    let pseudo_elements_path = manifest_dir.parent().unwrap().join("CSS/PseudoElements.json");
    let property_groups_path = manifest_dir
        .parent()
        .unwrap()
        .join("CSS/PseudoElementPropertyGroups.txt");
    println!("cargo:rerun-if-changed={}", pseudo_elements_path.display());
    println!("cargo:rerun-if-changed={}", property_groups_path.display());
    let pseudo_elements_value: serde_json::Value =
        serde_json::from_str(&std::fs::read_to_string(&pseudo_elements_path)?)?;
    let serde_json::Value::Object(pseudo_elements) = pseudo_elements_value else {
        return Err("PseudoElements.json does not contain a JSON object".into());
    };
    let property_groups = load_property_groups(&property_groups_path)?;
    let property_id = |name: &str| -> Result<u16, Box<dyn Error>> {
        if name == "custom" {
            return Ok(0);
        }
        ids.get(name)
            .copied()
            .ok_or_else(|| format!("unknown pseudo-element property '{name}'").into())
    };
    let property_ids = |entries: &[String]| -> Result<Vec<u16>, Box<dyn Error>> {
        let mut result = std::collections::BTreeSet::new();
        for entry in entries {
            if entry.starts_with("FIXME:") {
                continue;
            }
            if let Some(properties) = property_groups.get(entry) {
                for property in properties {
                    result.insert(property_id(property)?);
                }
            } else if entry.starts_with('#') {
                return Err(format!("unknown pseudo-element property group '{entry}'").into());
            } else {
                result.insert(property_id(entry)?);
            }
        }
        Ok(result.into_iter().collect())
    };
    let always_allowed_pseudo_properties = property_ids(
        property_groups
            .get("#always-allowed-properties")
            .ok_or("missing always-allowed pseudo-element property group")?,
    )?;
    let mut pseudo_property_whitelist_rows = Vec::new();
    for name in ordered_pseudo_element_names(&pseudo_elements)? {
        let object = pseudo_elements[&name].as_object().unwrap();
        let Some(whitelist) = object.get("property-whitelist") else {
            pseudo_property_whitelist_rows.push("    None,".to_string());
            continue;
        };
        let entries = whitelist
            .as_array()
            .unwrap()
            .iter()
            .map(|entry| entry.as_str().unwrap().to_string())
            .collect::<Vec<_>>();
        let whitelist = property_ids(&entries)?;
        pseudo_property_whitelist_rows.push(format!("    Some(&{whitelist:?}),"));
    }
    // UnknownWebKit follows the known pseudo-elements and accepts all properties.
    pseudo_property_whitelist_rows.push("    None,".to_string());

    // NB: Must match manually_specified_computation_order in
    //     Meta/Generators/generate_libweb_css_property_id.py; the parity test enforces it.
    let manual_order = [
        "math-depth",
        "font-family",
        "font-feature-settings",
        "font-kerning",
        "font-optical-sizing",
        "font-size",
        "font-style",
        "font-variant-alternates",
        "font-variant-caps",
        "font-variant-east-asian",
        "font-variant-emoji",
        "font-variant-ligatures",
        "font-variant-numeric",
        "font-variant-position",
        "font-variation-settings",
        "font-weight",
        "font-width",
        "text-rendering",
        "line-height",
        "color-scheme",
        "background-image",
        "direction",
        "writing-mode",
    ];
    let mut order = Vec::new();
    for name in manual_order {
        order.push(ids[name]);
    }
    // NB: The remainder follows Properties.json iteration order across all longhands,
    //     matching the C++ generator, not grouped by inheritance.
    for (name, value) in &properties {
        let object = value.as_object().unwrap();
        if object.contains_key("legacy-alias-for") || object.contains_key("longhands") || name == "all" {
            continue;
        }
        if !manual_order.contains(&name.as_str()) {
            order.push(ids[name]);
        }
    }

    let mut levels = vec![0u8; (last_longhand - first_longhand + 1) as usize];
    let mut animation_types = vec![0u8; levels.len()];
    let mut numeric_range_rows = vec![String::new(); levels.len()];
    let value_types = [
        "anchor",
        "anchor-size",
        "angle",
        "angle-percentage",
        "background-position",
        "basic-shape",
        "color",
        "corner-shape",
        "counter",
        "counter-style",
        "custom-ident",
        "dashed-ident",
        "easing-function",
        "filter-value-list",
        "fit-content",
        "flex",
        "font-style",
        "font-variant-alternates",
        "font-variant-east-asian",
        "font-variant-ligatures",
        "font-variant-numeric",
        "frequency",
        "frequency-percentage",
        "image",
        "integer",
        "length",
        "length-percentage",
        "number",
        "opacity-value",
        "opentype-tag",
        "paint",
        "percentage",
        "position",
        "ratio",
        "rect",
        "resolution",
        "scroll-function",
        "string",
        "time",
        "time-percentage",
        "transform-function",
        "transform-list",
        "url",
        "view-function",
        "view-timeline-inset",
    ];
    for name in inherited_longhands.iter().chain(&noninherited_longhands) {
        let index = (ids[name] - first_longhand) as usize;
        let requires_computation = property_field(name, "requires-computation").unwrap();
        let level = match requires_computation.as_str().unwrap() {
            "never" => 0u8,
            "cascaded-value" => 1,
            "non-inherited-value" => 2,
            "always" => 3,
            other => return Err(format!("unknown requires-computation '{other}' for {name}").into()),
        };
        levels[index] = level;

        animation_types[index] = match property_field(name, "animation-type").unwrap().as_str().unwrap() {
            "discrete" => 0,
            "by-computed-value" => 1,
            "repeatable-list" => 2,
            "custom" => 3,
            "none" => 4,
            other => return Err(format!("unknown animation-type '{other}' for {name}").into()),
        };

        let mut ranges = Vec::new();
        if let Some(valid_types) = property_field(name, "valid-types").and_then(|value| value.as_array().cloned()) {
            for valid_type in valid_types {
                let valid_type = valid_type.as_str().unwrap();
                let Some((type_name, range)) = valid_type.split_once(' ') else {
                    continue;
                };
                if !range.starts_with('[') || !range.ends_with(']') || !range.contains(',') {
                    continue;
                }
                if type_name == "custom-ident" {
                    continue;
                }
                let value_type = value_types
                    .iter()
                    .position(|candidate| *candidate == type_name)
                    .ok_or_else(|| format!("unknown ranged value type '{type_name}' for {name}"))?;
                let (min, max) = range[1..range.len() - 1]
                    .split_once(',')
                    .ok_or_else(|| format!("bad numeric range '{range}' for {name}"))?;
                let format_bound = |bound: &str| match (type_name, bound) {
                    ("integer", "-∞") => "i32::MIN as f64".to_string(),
                    ("integer", "∞") => "i32::MAX as f64".to_string(),
                    (_, "-∞") => "f32::MIN as f64".to_string(),
                    (_, "∞") => "f32::MAX as f64".to_string(),
                    _ if bound.contains('.') => bound.to_string(),
                    _ => format!("{bound}.0"),
                };
                ranges.push(format!(
                    "FfiPropertyNumericRange {{ value_type: {value_type}, min: {}, max: {} }}",
                    format_bound(min),
                    format_bound(max)
                ));
            }
        }
        numeric_range_rows[index] = ranges.join(", ");
    }

    let mut output = String::new();
    output.push_str("// Generated by build.rs from CSS metadata. Do not edit.\n\n");
    // Shorthand expansion tables for the cascade. The "all" shorthand expands to every
    // longhand except direction and unicode-bidi, mirroring the C++ generator.
    let mut shorthand_rows = Vec::new();
    for name in &shorthands {
        let longhand_names: Vec<String> = if name == "all" {
            properties
                .iter()
                .filter(|(name, value)| {
                    let object = value.as_object().unwrap();
                    !object.contains_key("longhands")
                        && !object.contains_key("legacy-alias-for")
                        && name.as_str() != "all"
                        && name.as_str() != "direction"
                        && name.as_str() != "unicode-bidi"
                })
                .map(|(name, _)| name.clone())
                .collect()
        } else {
            properties[name]["longhands"]
                .as_array()
                .unwrap()
                .iter()
                .map(|longhand| longhand.as_str().unwrap().to_string())
                .collect()
        };
        let ids_row: Vec<u16> = longhand_names.iter().map(|longhand| ids[longhand]).collect();
        shorthand_rows.push(format!("    &{ids_row:?},"));
    }
    output.push_str(&format!(
        "pub(crate) static SHORTHAND_EXPANSIONS: [&[u16]; {}] = [\n{}\n];\n\n",
        shorthand_rows.len(),
        shorthand_rows.join("\n")
    ));
    let property_names: Vec<&str> = shorthands
        .iter()
        .chain(&inherited_longhands)
        .chain(&noninherited_longhands)
        .map(String::as_str)
        .collect();
    fn expanded_longhands(
        name: &str,
        properties: &serde_json::Map<String, serde_json::Value>,
        all_longhands: &[String],
        result: &mut Vec<String>,
    ) {
        if name == "all" {
            result.extend_from_slice(all_longhands);
            return;
        }
        let Some(longhands) = properties[name].get("longhands").and_then(|value| value.as_array()) else {
            result.push(name.to_string());
            return;
        };
        for longhand in longhands {
            expanded_longhands(longhand.as_str().unwrap(), properties, all_longhands, result);
        }
    }
    let all_longhands: Vec<String> = inherited_longhands
        .iter()
        .chain(&noninherited_longhands)
        .filter(|name| name.as_str() != "direction" && name.as_str() != "unicode-bidi")
        .cloned()
        .collect();
    let expanded_shorthand_longhands: Vec<Vec<String>> = shorthands
        .iter()
        .map(|name| {
            let mut result = Vec::new();
            expanded_longhands(name, &properties, &all_longhands, &mut result);
            result
        })
        .collect();
    let camel_case_property_name = |name: &str| {
        let mut parts = name.split('-').filter(|part| !part.is_empty());
        let mut result = parts.next().unwrap_or_default().to_string();
        for part in parts {
            let mut characters = part.chars();
            if let Some(first) = characters.next() {
                result.extend(first.to_uppercase());
                result.extend(characters);
            }
        }
        result
    };
    let idl_names: Vec<String> = property_names
        .iter()
        .map(|name| camel_case_property_name(name))
        .collect();
    let logical_aliases: Vec<bool> = property_names
        .iter()
        .enumerate()
        .map(|(index, name)| {
            let name: &str = if index < shorthands.len() {
                expanded_shorthand_longhands[index][0].as_str()
            } else {
                name
            };
            properties[name].as_object().unwrap().contains_key("logical-alias-for")
        })
        .collect();
    let expanded_longhand_counts: Vec<usize> = expanded_shorthand_longhands.iter().map(Vec::len).collect();
    output.push_str(&format!(
        "pub(crate) static PROPERTY_IDL_NAMES: [&str; {}] = {:?};\n",
        idl_names.len(),
        idl_names
    ));
    output.push_str(&format!(
        "pub(crate) static PROPERTY_IS_LOGICAL_ALIAS: [bool; {}] = {:?};\n\n",
        logical_aliases.len(),
        logical_aliases
    ));
    output.push_str(&format!(
        "pub(crate) static SHORTHAND_EXPANDED_LONGHAND_COUNTS: [usize; {}] = {:?};\n\n",
        expanded_longhand_counts.len(),
        expanded_longhand_counts
    ));
    output.push_str(&format!(
        "pub const FIRST_SHORTHAND_PROPERTY_ID: u16 = {};\n",
        ids[&shorthands[0]]
    ));
    output.push_str(&format!(
        "pub const LAST_SHORTHAND_PROPERTY_ID: u16 = {};\n\n",
        ids[shorthands.last().unwrap()]
    ));
    output.push_str("#[allow(dead_code)]\npub mod property_id {\n");
    output.push_str("    pub const CUSTOM: u16 = 0;\n");
    for name in shorthands
        .iter()
        .chain(&inherited_longhands)
        .chain(&noninherited_longhands)
    {
        let constant = name.replace('-', "_").to_uppercase();
        output.push_str(&format!("    pub const {constant}: u16 = {};\n", ids[name]));
    }
    output.push_str("}\n\n");
    output.push_str(&format!(
        "pub const FIRST_LONGHAND_PROPERTY_ID: u16 = {first_longhand};\n"
    ));
    output.push_str(&format!(
        "pub const LAST_LONGHAND_PROPERTY_ID: u16 = {last_longhand};\n"
    ));
    output.push_str(&format!(
        "pub const FIRST_INHERITED_PROPERTY_ID: u16 = {first_inherited};\n"
    ));
    output.push_str(&format!(
        "pub const LAST_INHERITED_PROPERTY_ID: u16 = {last_inherited};\n\n"
    ));
    output.push_str(&format!(
        "pub(crate) static PROPERTY_COMPUTATION_ORDER: [u16; {}] = {:?};\n\n",
        order.len(),
        order
    ));
    output.push_str(&format!(
        "pub(crate) static REQUIRES_COMPUTATION_LEVELS: [u8; {}] = {:?};\n",
        levels.len(),
        levels
    ));
    output.push_str(&format!(
        "pub(crate) static PROPERTY_ANIMATION_TYPES: [u8; {}] = {:?};\n",
        animation_types.len(),
        animation_types
    ));
    output.push_str(&format!(
        "pub(crate) static PROPERTY_NUMERIC_RANGES: [&[FfiPropertyNumericRange]; {}] = [\n{}\n];\n",
        numeric_range_rows.len(),
        numeric_range_rows
            .iter()
            .map(|ranges| format!("    &[{ranges}],"))
            .collect::<Vec<_>>()
            .join("\n")
    ));
    output.push_str(&format!(
        "\npub(crate) static PSEUDO_ELEMENT_ALWAYS_ALLOWED_PROPERTIES: &[u16] = &{always_allowed_pseudo_properties:?};\n"
    ));
    output.push_str(&format!(
        "pub(crate) static PSEUDO_ELEMENT_PROPERTY_WHITELISTS: [Option<&[u16]>; {}] = [\n{}\n];\n",
        pseudo_property_whitelist_rows.len(),
        pseudo_property_whitelist_rows.join("\n")
    ));
    std::fs::write(out_dir.join("property_metadata_generated.rs"), output)?;
    Ok(())
}

// Generates the length unit table for the Rust style computation core from Units.json.
// Unit codes are the alphabetical index within the "length" object, matching the C++
// LengthUnit enum; absolute units carry their canonical px ratio.
fn generate_length_units(manifest_dir: &Path, out_dir: &Path) -> Result<(), Box<dyn Error>> {
    let units_path = manifest_dir.parent().unwrap().join("CSS/Units.json");
    println!("cargo:rerun-if-changed={}", units_path.display());
    let value: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&units_path)?)?;
    let lengths = value["length"].as_object().ok_or("Units.json has no length object")?;

    let mut names = Vec::new();
    let mut ratios = Vec::new();
    for (name, unit) in lengths {
        names.push(name.clone());
        let object = unit.as_object().unwrap();
        if object
            .get("is-canonical-unit")
            .is_some_and(|canonical| canonical.as_bool() == Some(true))
        {
            ratios.push("1.0".to_string());
        } else if let Some(ratio) = object.get("number-of-canonical-unit") {
            ratios.push(format!("{:?}", ratio.as_f64().unwrap()));
        } else {
            ratios.push("f64::NAN".to_string());
        }
    }

    let mut output = String::new();
    output.push_str("// Generated by build.rs from Units.json. Do not edit.\n\n");
    output.push_str(&format!(
        "pub(crate) static LENGTH_UNIT_NAMES: [&str; {}] = {:?};\n\n",
        names.len(),
        names
    ));
    output.push_str(&format!(
        "pub(crate) static LENGTH_UNIT_CANONICAL_PX_RATIOS: [f64; {}] = [{}];\n",
        ratios.len(),
        ratios.join(", ")
    ));
    std::fs::write(out_dir.join("length_units_generated.rs"), output)?;

    // Canonical-unit ratios for the other dimensions, indexed by unit code in
    // JSON order like the C++ unit enums. Relative units (none exist outside
    // lengths) would be NaN.
    let mut output = String::from("// Generated by build.rs from Units.json. Do not edit.\n");
    for dimension in ["angle", "flex", "frequency", "resolution", "time"] {
        let units = value[dimension]
            .as_object()
            .ok_or_else(|| format!("Units.json has no {dimension} object"))?;
        let mut ratios = Vec::new();
        for (_, unit) in units {
            let object = unit.as_object().unwrap();
            if object
                .get("is-canonical-unit")
                .is_some_and(|canonical| canonical.as_bool() == Some(true))
            {
                ratios.push("1.0".to_string());
            } else if let Some(ratio) = object.get("number-of-canonical-unit") {
                ratios.push(format!("{:?}", ratio.as_f64().unwrap()));
            } else {
                ratios.push("f64::NAN".to_string());
            }
        }
        output.push_str(&format!(
            "\npub(crate) static {}_UNIT_CANONICAL_RATIOS: [f64; {}] = [{}];\n",
            dimension.to_uppercase(),
            ratios.len(),
            ratios.join(", ")
        ));
        let names: Vec<&str> = units.keys().map(String::as_str).collect();
        output.push_str(&format!(
            "\n#[allow(dead_code)]\npub(crate) static {}_UNIT_NAMES: [&str; {}] = {:?};\n",
            dimension.to_uppercase(),
            names.len(),
            names
        ));
    }
    std::fs::write(out_dir.join("dimension_units_generated.rs"), output)?;
    Ok(())
}

// Generates keyword codes for the Rust style computation core from Keywords.json.
// Invalid is 0 and the codes follow the array order, matching the C++ Keyword enum.
fn generate_keywords(manifest_dir: &Path, out_dir: &Path) -> Result<(), Box<dyn Error>> {
    let keywords_path = manifest_dir.parent().unwrap().join("CSS/Keywords.json");
    println!("cargo:rerun-if-changed={}", keywords_path.display());
    let value: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&keywords_path)?)?;
    let keywords = value.as_array().ok_or("Keywords.json is not an array")?;

    let mut output = String::new();
    output.push_str("// Generated by build.rs from Keywords.json. Do not edit.\n\n");
    output.push_str("#[allow(dead_code)]\npub mod keyword {\n");
    output.push_str("    pub const INVALID: u16 = 0;\n");
    for (index, keyword) in keywords.iter().enumerate() {
        let name = keyword.as_str().unwrap();
        let constant = name.replace('-', "_").to_uppercase();
        output.push_str(&format!("    pub const {constant}: u16 = {};\n", index + 1));
    }
    output.push_str("}\n");
    std::fs::write(out_dir.join("keywords_generated.rs"), output)?;
    Ok(())
}

// Generates enum value codes for the Rust style computation core from Enums.json.
// Values follow the array order skipping aliases, matching the C++ enum generator.
fn generate_css_enums(manifest_dir: &Path, out_dir: &Path) -> Result<(), Box<dyn Error>> {
    let enums_path = manifest_dir.parent().unwrap().join("CSS/Enums.json");
    println!("cargo:rerun-if-changed={}", enums_path.display());
    let value: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&enums_path)?)?;
    let enums = value.as_object().ok_or("Enums.json is not an object")?;

    let mut output = String::new();
    output.push_str("// Generated by build.rs from Enums.json. Do not edit.\n\n");
    for (name, values) in enums {
        let values = values.as_array().ok_or("enum values are not an array")?;
        let module = name.replace('-', "_");
        output.push_str(&format!("#[allow(dead_code)]\npub mod {module} {{\n"));
        let mut code: u8 = 0;
        for value in values {
            let value = value.as_str().ok_or("enum value is not a string")?;
            // Aliases are not included in the enum.
            if value.contains('=') {
                continue;
            }
            let constant = value.replace('-', "_").to_uppercase();
            output.push_str(&format!("    pub const {constant}: u8 = {code};\n"));
            code = code.checked_add(1).ok_or("enum has too many values for u8")?;
        }
        output.push_str("}\n\n");

        // The keyword-to-enum-value mapping, mirroring the C++ keyword_to_<enum> generator,
        // including its alias handling ("keyword=member" maps the keyword to member's value).
        let mut member_codes = std::collections::HashMap::new();
        let mut member_code: u8 = 0;
        for value in values {
            let value = value.as_str().ok_or("enum value is not a string")?;
            if value.contains('=') {
                continue;
            }
            member_codes.insert(value.to_string(), member_code);
            member_code = member_code.checked_add(1).ok_or("enum has too many values for u8")?;
        }
        output.push_str(&format!(
            "#[allow(dead_code)]\npub fn keyword_to_{module}(keyword: u16) -> Option<u8> {{\n    match keyword {{\n"
        ));
        for value in values {
            let value = value.as_str().ok_or("enum value is not a string")?;
            let (keyword_name, member_name) = match value.split_once('=') {
                Some((keyword, member)) => (keyword, member),
                None => (value, value),
            };
            let keyword_constant = keyword_name.replace('-', "_").to_uppercase();
            let code = member_codes
                .get(member_name)
                .ok_or("enum alias targets an unknown member")?;
            output.push_str(&format!("        keyword::{keyword_constant} => Some({code}),\n"));
        }
        output.push_str("        _ => None,\n    }\n}\n\n");
    }
    std::fs::write(out_dir.join("css_enums_generated.rs"), output)?;
    Ok(())
}

fn generate_ffi_header(
    config: cbindgen::Config,
    sources: &[PathBuf],
    out_dir: &Path,
    ffi_out_dir: &Path,
    header: &Path,
) {
    let builder = sources
        .iter()
        .fold(cbindgen::Builder::new().with_config(config), |builder, source| {
            builder.with_src(source)
        });
    builder.generate().map_or_else(
        |error| match error {
            cbindgen::Error::ParseSyntaxError { .. } => {}
            other => panic!("{other:?}"),
        },
        |bindings| {
            let output_header = out_dir.join(header);
            std::fs::create_dir_all(output_header.parent().unwrap()).unwrap();
            bindings.write_to_file(output_header);
            if ffi_out_dir != out_dir {
                let ffi_header = ffi_out_dir.join(header);
                std::fs::create_dir_all(ffi_header.parent().unwrap()).unwrap();
                bindings.write_to_file(ffi_header);
            }
        },
    );
}

/// Strict variant for the layout headers: dormant layout sources are parsed
/// by cbindgen before they join the module tree, so a syntax error anywhere
/// must fail the build instead of silently producing a stale header.
// CssPixels is a single i32 fixed-point value shared with the CSS module; C++
// already has the matching CSSPixels class, so expose its raw representation in
// the generated ABI rather than generating a second C++ pixel type in the
// RustFFI namespace.
fn expose_css_pixels_as_raw_i32(config: &mut cbindgen::Config) {
    config.export.exclude.push("CssPixels".to_string());
    config
        .export
        .rename
        .insert("CssPixels".to_string(), "int32_t".to_string());
}

fn generate_ffi_header_strict(
    config: cbindgen::Config,
    sources: &[PathBuf],
    out_dir: &Path,
    ffi_out_dir: &Path,
    header: &Path,
) {
    let builder = sources
        .iter()
        .fold(cbindgen::Builder::new().with_config(config), |builder, source| {
            builder.with_src(source)
        });
    builder.generate().map_or_else(
        |error| panic!("{error}"),
        |bindings| {
            let output_header = out_dir.join(header);
            std::fs::create_dir_all(output_header.parent().unwrap()).unwrap();
            bindings.write_to_file(output_header);
            if ffi_out_dir != out_dir {
                let ffi_header = ffi_out_dir.join(header);
                std::fs::create_dir_all(ffi_header.parent().unwrap()).unwrap();
                bindings.write_to_file(ffi_header);
            }
        },
    );
}

fn main() -> Result<(), Box<dyn Error>> {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let out_dir = PathBuf::from(env::var("OUT_DIR")?);

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=cbindgen.toml");
    println!("cargo:rerun-if-env-changed=FFI_OUTPUT_DIR");
    println!("cargo:rerun-if-changed=src");

    generate_selector_pseudo_types(&manifest_dir, &out_dir)?;
    generate_property_metadata(&manifest_dir, &out_dir)?;
    generate_length_units(&manifest_dir, &out_dir)?;
    generate_keywords(&manifest_dir, &out_dir)?;
    generate_css_enums(&manifest_dir, &out_dir)?;

    let ffi_out_dir = env::var("FFI_OUTPUT_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| out_dir.clone());

    let base_config = cbindgen::Config::from_file(manifest_dir.join("cbindgen.toml"))?;

    // CSS tokenizer header — namespace Web::CSS::Parser::FFI, CSS types only.
    let mut css_config = base_config.clone();
    css_config.namespaces = Some(vec![
        "Web".to_string(),
        "CSS".to_string(),
        "Parser".to_string(),
        "FFI".to_string(),
    ]);
    css_config.export.include = vec![
        "CssHashType".to_string(),
        "CssNumberType".to_string(),
        "CssToken".to_string(),
        "CssTokenType".to_string(),
    ];

    generate_ffi_header(
        css_config,
        &[manifest_dir.join("src/css/css_tokenizer.rs")],
        &out_dir,
        &ffi_out_dir,
        Path::new("RustFFI.h"),
    );

    // Selector matching header - namespace Web::CSS::SelectorFFI. Generate both sides of this
    // ABI from the Rust declarations so changes to layouts or signatures cannot drift silently.
    let mut selector_config = base_config.clone();
    selector_config.namespaces = Some(vec!["Web".to_string(), "CSS".to_string(), "SelectorFFI".to_string()]);
    for (rust_name, cxx_name) in [
        ("FfiSimpleSelectorType", "SimpleSelectorType"),
        ("FfiDirection", "Direction"),
        ("FfiPseudoElementValueType", "PseudoElementValueType"),
        ("FfiStringView", "StringView"),
        ("FfiSimpleSelector", "SimpleSelector"),
        ("FfiCompoundSelector", "CompoundSelector"),
        ("FfiSelector", "Selector"),
        ("FfiElement", "Element"),
        ("FfiElementQualifiedName", "ElementQualifiedName"),
        ("FfiInternedStringList", "InternedStringList"),
        ("FfiDomStringView", "DomStringView"),
        ("FfiDomAttribute", "DomAttribute"),
        ("FfiResolvedNamespaceType", "ResolvedNamespaceType"),
        ("FfiResolvedNamespace", "ResolvedNamespace"),
        ("FfiElementAndShadowHost", "ElementAndShadowHost"),
    ] {
        selector_config
            .export
            .rename
            .insert(rust_name.to_string(), cxx_name.to_string());
    }

    generate_ffi_header(
        selector_config,
        &[
            manifest_dir.join("src/css/selector_engine.rs"),
            manifest_dir.join("src/css/ffi_support.rs"),
        ],
        &out_dir,
        &ffi_out_dir,
        Path::new("SelectorRustFFI.h"),
    );

    // Style value header - namespace Web::CSS::StyleValueFFI. The StyleValueData layout is
    // exposed so converted C++ StyleValue subclasses can read variant payloads inline without
    // an FFI call.
    let mut style_value_config = base_config.clone();
    style_value_config.namespaces = Some(vec!["Web".to_string(), "CSS".to_string(), "StyleValueFFI".to_string()]);
    style_value_config.export.include = vec!["StyleValueData".to_string()];

    generate_ffi_header(
        style_value_config,
        &[
            manifest_dir.join("src/css/style_value.rs"),
            manifest_dir.join("src/css/retained_fly_string.rs"),
            manifest_dir.join("src/css/color_interpolation.rs"),
            manifest_dir.join("src/css/animation.rs"),
            manifest_dir.join("src/css/transition.rs"),
            manifest_dir.join("src/css/calc.rs"),
            manifest_dir.join("src/css/ffi_stats.rs"),
        ],
        &out_dir,
        &ffi_out_dir,
        Path::new("StyleValueRustFFI.h"),
    );

    // Computed values header - namespace Web::CSS::ComputedValuesFFI. Exposes the style group
    // vtable and lifecycle functions; the reference count header layout is documented in
    // computed_values.rs and mirrored by StyleStructRef.
    let mut computed_values_config = base_config.clone();
    computed_values_config.namespaces = Some(vec![
        "Web".to_string(),
        "CSS".to_string(),
        "ComputedValuesFFI".to_string(),
    ]);
    computed_values_config.export.include = vec![
        "StyleGroupVTable".to_string(),
        "STYLE_GROUP_STATIC_REFCOUNT".to_string(),
        "GRID_NO_INDEX".to_string(),
        "ComputedGridTrackEntryKind".to_string(),
        "ComputedGridPlacementKind".to_string(),
        "CascadeOrigin".to_string(),
    ];

    generate_ffi_header(
        computed_values_config,
        &[
            manifest_dir.join("src/css/computed_values.rs"),
            manifest_dir.join("src/css/property_metadata.rs"),
            manifest_dir.join("src/css/style_compute.rs"),
            manifest_dir.join("src/css/display.rs"),
            manifest_dir.join("src/css/computed_value_types.rs"),
            manifest_dir.join("src/css/retained_fly_string.rs"),
            manifest_dir.join("src/css/css_pixels.rs"),
            manifest_dir.join("src/css/cascaded_properties.rs"),
            manifest_dir.join("src/css/custom_properties.rs"),
        ],
        &out_dir,
        &ffi_out_dir,
        Path::new("ComputedValuesRustFFI.h"),
    );

    // Encoding-detection header - namespace Web::HTML::Parser, rust_detect_encoding only.
    let mut html_config = base_config.clone();
    html_config.namespaces = Some(vec!["Web".to_string(), "HTML".to_string(), "Parser".to_string()]);
    html_config.export.include = vec!["rust_detect_encoding".to_string()];

    generate_ffi_header(
        html_config,
        &[manifest_dir.join("src/encoding_detection.rs")],
        &out_dir,
        &ffi_out_dir,
        Path::new("HTML/Parser/RustFFI.h"),
    );

    // Layout tree-builder header - namespace Web::Layout::RustFFI. The node arena and its
    // node records are read directly by the C++ layout tree wrappers.
    let mut tree_builder_config = base_config.clone();
    tree_builder_config.namespaces = Some(vec!["Web".to_string(), "Layout".to_string(), "RustFFI".to_string()]);
    tree_builder_config.export.include = vec![
        "FfiNodeKindFacts".to_string(),
        "FfiReplacedContentFacts".to_string(),
        "FfiStylePayloads".to_string(),
        "NodeAllocation".to_string(),
        "NodeData".to_string(),
        "NodeFlag".to_string(),
        "NodeKind".to_string(),
        "NodeSlotId".to_string(),
    ];
    tree_builder_config.export.exclude = vec![
        "rust_calc_node_create_numeric_dimension".to_string(),
        "rust_calc_resolve".to_string(),
    ];
    expose_css_pixels_as_raw_i32(&mut tree_builder_config);
    generate_ffi_header_strict(
        tree_builder_config,
        &[
            manifest_dir.join("src/layout/layout_node_arena.rs"),
            manifest_dir.join("src/layout/node_data.rs"),
            manifest_dir.join("src/layout/tree_builder.rs"),
            manifest_dir.join("../../RustAllocator.rs"),
        ],
        &out_dir,
        &ffi_out_dir,
        Path::new("Layout/TreeBuilderRustFFI.h"),
    );

    // Layout engine header - namespace Web::Layout::RustFFI, layered on the tree-builder
    // header.
    let mut layout_config = base_config;
    layout_config.namespaces = Some(vec!["Web".to_string(), "Layout".to_string(), "RustFFI".to_string()]);
    layout_config.export.exclude = vec![
        "rust_calc_node_create_numeric_dimension".to_string(),
        "rust_calc_resolve".to_string(),
        "rust_calc_root_from_calculated".to_string(),
        "rust_calc_external_resolutions".to_string(),
        "rust_calc_external_resolutions_release".to_string(),
    ];
    expose_css_pixels_as_raw_i32(&mut layout_config);
    layout_config
        .includes
        .push("LibWeb/Layout/TreeBuilderRustFFI.h".to_string());
    layout_config.export.include = vec![
        "FfiGridTrackType".to_string(),
        "FfiGridTrackState".to_string(),
        "FfiFormattingContextType".to_string(),
        "FfiFlexLayoutClampState".to_string(),
        "FfiFlexLayoutGrowthState".to_string(),
    ];
    generate_ffi_header_strict(
        layout_config,
        &[
            manifest_dir.join("src/layout/used_values.rs"),
            manifest_dir.join("src/layout/commit.rs"),
            manifest_dir.join("src/layout/geometry.rs"),
            manifest_dir.join("src/layout/style_values.rs"),
            manifest_dir.join("src/layout/node_facts.rs"),
            manifest_dir.join("src/css/computed_value_types.rs"),
            manifest_dir.join("src/css/display.rs"),
            manifest_dir.join("src/layout/formatting_context.rs"),
            manifest_dir.join("src/layout/flex_formatting_context.rs"),
            manifest_dir.join("src/layout/grid_formatting_context.rs"),
            manifest_dir.join("src/layout/svg_formatting_context.rs"),
            manifest_dir.join("src/layout/table_formatting_context.rs"),
            manifest_dir.join("src/layout/inline_formatting_context.rs"),
            manifest_dir.join("src/layout/inline_level_iterator.rs"),
            manifest_dir.join("src/layout/line_builder.rs"),
            manifest_dir.join("src/layout/line_box.rs"),
            manifest_dir.join("src/layout/line_box_fragment.rs"),
        ],
        &out_dir,
        &ffi_out_dir,
        Path::new("Layout/LayoutRustFFI.h"),
    );

    Ok(())
}
