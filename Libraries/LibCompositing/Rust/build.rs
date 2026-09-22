/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::env;
use std::error::Error;
use std::path::{Path, PathBuf};

fn generate_ffi_header(config: cbindgen::Config, sources: &[PathBuf], out_dir: &Path, header: &Path) {
    let builder = sources
        .iter()
        .fold(cbindgen::Builder::new().with_config(config), |builder, source| {
            builder.with_src(source)
        });
    builder.generate().map_or_else(
        |error| match error {
            cbindgen::Error::ParseSyntaxError { .. } => {
                // Do nothing, the build will fail later with a nicer error message when compiling with rustc
            }
            other => panic!("{other:?}"),
        },
        |bindings| {
            let output_header = out_dir.join(header);
            std::fs::create_dir_all(output_header.parent().unwrap()).unwrap();
            bindings.write_to_file(output_header);
        },
    );
}

fn generate_ffi_header_strict(config: cbindgen::Config, sources: &[PathBuf], out_dir: &Path, header: &Path) {
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
        },
    );
}

fn expose_css_pixel_types_as_web_types(config: &mut cbindgen::Config) {
    for (rust_name, cpp_name) in [
        ("CssPixels", "Compositing::CSSPixels"),
        ("FfiCssPixelPoint", "Compositing::CSSPixelPoint"),
        ("FfiCssPixelSize", "Compositing::CSSPixelSize"),
        ("FfiCssPixelRect", "Compositing::CSSPixelRect"),
    ] {
        config.export.exclude.push(rust_name.to_string());
        config.export.rename.insert(rust_name.to_string(), cpp_name.to_string());
    }
    config.includes.push("LibCompositing/PixelUnits.h".to_string());
    config.after_includes = Some(
        "#if defined(__clang__)\n#pragma clang diagnostic push\n#pragma clang diagnostic ignored \"-Wreturn-type-c-linkage\"\n#endif"
            .to_string(),
    );
    config.trailer = Some("#if defined(__clang__)\n#pragma clang diagnostic pop\n#endif".to_string());
}

// The types the C++ side defines itself, which the generated header refers to by their C++ names.
fn expose_shared_abi_types_as_cpp_types(config: &mut cbindgen::Config) {
    config.export.exclude.extend(
        [
            "OptionalFloatRect",
            "OptionalColor",
            "OptionalU32",
            "OptionalF32",
            "OptionalAffineTransform",
            "OptionalCssPixels",
            "OptionalCssPixelRect",
            "OptionalIntRect",
            "OptionalFloatPoint",
            "OptionalFloatSize",
            "OptionalI64",
            "OptionalUsize",
            "ClipMode",
            "SpatialNodeIndex",
            "ClipNodeIndex",
            "EffectNodeIndex",
            "ContextRef",
            "DisplayListCommandRun",
            "ReplayClip",
            "ReplayLayer",
            "ReplayMask",
            "FfiVisualViewportTransform",
        ]
        .map(String::from),
    );
    for (rust_name, cpp_name) in [
        ("IntPoint", "Gfx::IntPoint"),
        ("FloatPoint", "Gfx::FloatPoint"),
        ("IntSize", "Gfx::IntSize"),
        ("FloatSize", "Gfx::FloatSize"),
        ("FloatVector3", "Gfx::FloatVector3"),
        ("IntRect", "Gfx::IntRect"),
        ("FloatRect", "Gfx::FloatRect"),
        ("Color", "Gfx::Color"),
        ("AffineTransform", "Gfx::AffineTransform"),
        ("FloatMatrix4x4", "Gfx::FloatMatrix4x4"),
        ("CornerRadius", "Gfx::CornerRadius"),
        ("CornerRadii", "Gfx::CornerRadii"),
        ("GradientInterpolationMethod", "Gfx::GradientInterpolationMethod"),
        ("WindingRule", "Gfx::WindingRule"),
        ("MaskKind", "Gfx::MaskKind"),
        ("CompositingAndBlendingOperator", "Gfx::CompositingAndBlendingOperator"),
        ("ColorFilterType", "Gfx::ColorFilterType"),
        ("ScalingMode", "Gfx::ScalingMode"),
        ("InterpolationColorSpace", "Gfx::InterpolationColorSpace"),
        ("OptionalFloatRect", "Optional<Gfx::FloatRect>"),
        ("OptionalColor", "Optional<Gfx::Color>"),
        ("OptionalU32", "Optional<u32>"),
        ("OptionalF32", "Optional<float>"),
        ("OptionalAffineTransform", "Optional<Gfx::AffineTransform>"),
        ("OptionalCssPixels", "Optional<Compositing::CSSPixels>"),
        ("OptionalCssPixelRect", "Optional<Compositing::CSSPixelRect>"),
        ("OptionalIntRect", "Optional<Gfx::IntRect>"),
        ("OptionalFloatPoint", "Optional<Gfx::FloatPoint>"),
        ("OptionalFloatSize", "Optional<Gfx::FloatSize>"),
        ("OptionalI64", "Optional<i64>"),
        ("OptionalUsize", "Optional<size_t>"),
        ("ClipMode", "Compositing::ClipMode"),
        ("SpatialNodeIndex", "Compositing::SpatialNodeIndex"),
        ("ClipNodeIndex", "Compositing::ClipNodeIndex"),
        ("EffectNodeIndex", "Compositing::EffectNodeIndex"),
        ("ContextRef", "Compositing::ContextRef"),
        ("DisplayListCommandRun", "Compositing::DisplayListCommandRun"),
        ("ReplayClip", "Compositing::ReplayClip"),
        ("ReplayLayer", "Compositing::ReplayLayer"),
        ("ReplayMask", "Compositing::ReplayMask"),
        ("FfiVisualViewportTransform", "Compositing::TransformWithOrigin"),
    ] {
        config.export.rename.insert(rust_name.to_string(), cpp_name.to_string());
    }

    config.includes.extend(
        [
            "AK/Optional.h",
            "LibGfx/AffineTransform.h",
            "LibGfx/Color.h",
            "LibGfx/CompositingAndBlendingOperator.h",
            "LibGfx/CornerRadii.h",
            "LibGfx/Filter.h",
            "LibGfx/GradientInterpolation.h",
            "LibGfx/InterpolationColorSpace.h",
            "LibGfx/Matrix4x4.h",
            "LibGfx/Point.h",
            "LibGfx/Rect.h",
            "LibGfx/ScalingMode.h",
            "LibGfx/Size.h",
            "LibGfx/Vector3.h",
            "LibGfx/WindingRule.h",
            "LibCompositing/DisplayList/AccumulatedVisualContext.h",
            "LibCompositing/Forward.h",
            "LibCompositing/DisplayList/DisplayListCommandsGenerated.h",
        ]
        .map(String::from),
    );
}

fn public_type_names(path: &Path) -> Result<Vec<String>, Box<dyn Error>> {
    println!("cargo:rerun-if-changed={}", path.display());
    let mut names = Vec::new();
    for line in std::fs::read_to_string(path)?.lines() {
        for prefix in ["pub struct ", "pub enum "] {
            if let Some(rest) = line.strip_prefix(prefix) {
                let name: String = rest.chars().take_while(|c| c.is_alphanumeric() || *c == '_').collect();
                if !name.is_empty() {
                    names.push(name);
                }
            }
        }
    }
    Ok(names)
}

fn display_list_command_names(path: &Path) -> Result<Vec<String>, Box<dyn Error>> {
    let source = std::fs::read_to_string(path)?;
    let mut names = Vec::new();
    for line in source.lines() {
        let Some(rest) = line.strip_prefix("impl DisplayListCommand for ") else {
            continue;
        };
        let name: String = rest.chars().take_while(|c| c.is_alphanumeric() || *c == '_').collect();
        if name.is_empty() {
            return Err(format!("unparsable DisplayListCommand impl header: {line}").into());
        }
        names.push(name);
    }
    if names.is_empty() {
        return Err("no DisplayListCommand impls found".into());
    }
    Ok(names)
}

fn main() -> Result<(), Box<dyn Error>> {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let out_dir = PathBuf::from(env::var("OUT_DIR")?);

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=cbindgen.toml");
    println!("cargo:rerun-if-changed=src");

    let base_config = cbindgen::Config::from_file(manifest_dir.join("cbindgen.toml"))?;

    // The entry points and the plain values that cross the boundary - namespace Compositing::RustFFI.
    let mut ffi_config = base_config.clone();
    ffi_config.namespaces = Some(vec!["Compositing".to_string(), "RustFFI".to_string()]);
    expose_css_pixel_types_as_web_types(&mut ffi_config);
    expose_shared_abi_types_as_cpp_types(&mut ffi_config);
    // The values WebContent's own entry points take and return, which its headers refer to here.
    ffi_config.export.include = [
        "NodeSlotId",
        "FfiFilterFunction",
        "FfiFilterFunctionKind",
        "FfiVisualContextTreeInputs",
        "FfiVisualAnimationTargetKind",
        "FfiVisualAnimationPlaybackDirection",
        "FfiVisualAnimationFillMode",
        "FfiVisualAnimationTransformOperationKind",
        "FfiCompositorAnimationPublishOutcome",
        "FfiVisualAnimationSummary",
        "FfiAnimatedContentViewportEffect",
        "FfiTestStickyConstraints",
        "FfiRecordedDisplayList",
        "FfiDisplayListReplayCallbacks",
        "FfiEasingDescriptor",
        "FfiEasingKind",
        "FfiLinearEasingPoint",
    ]
    .map(String::from)
    .to_vec();
    generate_ffi_header(
        ffi_config,
        &[
            manifest_dir.join("src/node_slot_id.rs"),
            manifest_dir.join("src/css_pixels.rs"),
            manifest_dir.join("src/easing.rs"),
            manifest_dir.join("src/filter_bytes.rs"),
            manifest_dir.join("src/visual_context/ffi_types.rs"),
            manifest_dir.join("src/host/replay.rs"),
            manifest_dir.join("src/display_list/storage.rs"),
            manifest_dir.join("src/ffi.rs"),
        ],
        &out_dir,
        Path::new("RustFFI.h"),
    );

    // The mirrors of the LibGfx value types, which the C++ side checks its own layouts against.
    let mut types_config = base_config.clone();
    types_config.namespaces = Some(vec!["Compositing".to_string(), "RustFFI".to_string()]);
    expose_css_pixel_types_as_web_types(&mut types_config);
    let libgfx_src_dir = manifest_dir.join("../../LibGfx/Rust/src");
    // Every type declared in these is exported; `commands.rs` is parsed only so cbindgen can
    // resolve the signatures that mention its types, which are defined in C++.
    let libgfx_type_sources = [
        libgfx_src_dir.join("geometry.rs"),
        libgfx_src_dir.join("matrix.rs"),
        libgfx_src_dir.join("color.rs"),
        libgfx_src_dir.join("corner_radii.rs"),
        libgfx_src_dir.join("paint_enums.rs"),
    ];
    let commands_source = manifest_dir.join("src/display_list/commands.rs");
    let types_sources: Vec<PathBuf> = libgfx_type_sources
        .iter()
        .cloned()
        .chain([commands_source.clone()])
        .collect();
    types_config.export.include = Vec::new();
    for libgfx_source in &libgfx_type_sources {
        types_config.export.include.extend(public_type_names(libgfx_source)?);
    }
    types_config.export.include.extend(
        [
            "OptionalFloatRect",
            "OptionalColor",
            "OptionalU32",
            "OptionalF32",
            "OptionalAffineTransform",
            "FontResourceId",
            "ImageFrameResourceId",
            "VideoSinkResourceId",
            "DisplayListResourceId",
            "CanvasId",
            "CompositorContextId",
            "UniqueNodeId",
        ]
        .map(String::from),
    );
    generate_ffi_header_strict(types_config, &types_sources, &out_dir, Path::new("TypesRustFFI.h"));

    // The display list commands, which are the same struct on both sides of the boundary.
    let mut commands_config = base_config;
    commands_config.layout.aligned_n = Some("alignas".to_string());
    commands_config.namespaces = Some(vec!["Compositing".to_string()]);
    let types_with_existing_cpp_definitions = [
        "SpatialNodeIndex",
        "ClipNodeIndex",
        "EffectNodeIndex",
        "ContextRef",
        "FontResourceId",
        "ImageFrameResourceId",
        "VideoSinkResourceId",
        "DisplayListResourceId",
        "CanvasId",
        "CompositorContextId",
        "UniqueNodeId",
        "OptionalFloatRect",
        "OptionalColor",
        "OptionalU32",
        "OptionalF32",
        "OptionalAffineTransform",
        "VISUAL_VIEWPORT_NODE_INDEX",
        "DISPLAY_LIST_COMMAND_TYPE_COUNT",
    ];
    commands_config.export.include = public_type_names(&commands_source)?
        .into_iter()
        .filter(|name| !types_with_existing_cpp_definitions.contains(&name.as_str()))
        .collect();
    commands_config.export.exclude = types_with_existing_cpp_definitions
        .iter()
        .map(|name| name.to_string())
        .collect();
    let references_renamed_to_real_types = [
        ("IntPoint", "Gfx::IntPoint"),
        ("FloatPoint", "Gfx::FloatPoint"),
        ("IntSize", "Gfx::IntSize"),
        ("FloatSize", "Gfx::FloatSize"),
        ("IntRect", "Gfx::IntRect"),
        ("FloatRect", "Gfx::FloatRect"),
        ("Color", "Gfx::Color"),
        ("AffineTransform", "Gfx::AffineTransform"),
        ("FloatMatrix4x4", "Gfx::FloatMatrix4x4"),
        ("CornerRadius", "Gfx::CornerRadius"),
        ("CornerRadii", "Gfx::CornerRadii"),
        ("CornerClip", "Gfx::CornerClip"),
        ("GradientInterpolationMethod", "Gfx::GradientInterpolationMethod"),
        ("GradientInterpolationType", "Gfx::GradientInterpolationMethod::Type"),
        ("RectangularColorSpace", "Gfx::RectangularColorSpace"),
        ("PolarColorSpace", "Gfx::PolarColorSpace"),
        ("HueInterpolationMethod", "Gfx::HueInterpolationMethod"),
        ("InterpolationColorSpace", "Gfx::InterpolationColorSpace"),
        ("WindingRule", "Gfx::WindingRule"),
        ("LineStyle", "Gfx::LineStyle"),
        ("CapStyle", "Gfx::Path::CapStyle"),
        ("JoinStyle", "Gfx::Path::JoinStyle"),
        ("ScalingMode", "Gfx::ScalingMode"),
        ("CompositingAndBlendingOperator", "Gfx::CompositingAndBlendingOperator"),
        ("ColorFilterType", "Gfx::ColorFilterType"),
        ("Orientation", "Gfx::Orientation"),
        ("MaskKind", "Gfx::MaskKind"),
        ("ShouldAntiAlias", "Gfx::ShouldAntiAlias"),
        ("CssPixels", "Compositing::CSSPixels"),
        ("FfiCssPixelPoint", "Compositing::CSSPixelPoint"),
        ("FfiCssPixelRect", "Compositing::CSSPixelRect"),
        ("OptionalFloatRect", "Optional<Gfx::FloatRect>"),
        ("OptionalColor", "Optional<Gfx::Color>"),
        ("OptionalU32", "Optional<u32>"),
        ("OptionalF32", "Optional<float>"),
        ("OptionalAffineTransform", "Optional<Gfx::AffineTransform>"),
        ("CompositorContextId", "Compositing::CompositorContextId"),
        ("UniqueNodeId", "UniqueNodeID"),
    ];
    for (rust_name, cpp_name) in references_renamed_to_real_types {
        commands_config
            .export
            .rename
            .insert(rust_name.to_string(), cpp_name.to_string());
    }
    commands_config.includes = [
        "AK/Forward.h",
        "AK/Optional.h",
        "AK/Types.h",
        "LibGfx/AffineTransform.h",
        "LibGfx/AntiAliasing.h",
        "LibGfx/Color.h",
        "LibGfx/CompositingAndBlendingOperator.h",
        "LibGfx/CornerRadii.h",
        "LibGfx/Forward.h",
        "LibGfx/GradientInterpolation.h",
        "LibGfx/InterpolationColorSpace.h",
        "LibGfx/LineStyle.h",
        "LibGfx/Orientation.h",
        "LibGfx/Path.h",
        "LibGfx/Point.h",
        "LibGfx/Rect.h",
        "LibGfx/ScalingMode.h",
        "LibGfx/Size.h",
        "LibGfx/WindingRule.h",
        "LibCompositing/DisplayList/ContextRef.h",
        "LibCompositing/DisplayList/DisplayListResourceIds.h",
        "LibCompositing/Forward.h",
        "LibCompositing/PixelUnits.h",
        "LibCompositing/Types.h",
    ]
    .into_iter()
    .map(String::from)
    .collect();
    for name in display_list_command_names(&commands_source)? {
        commands_config.export.pre_body.insert(
            name.clone(),
            format!("    static constexpr DisplayListCommandType command_type = DisplayListCommandType::{name};"),
        );
    }
    generate_ffi_header_strict(
        commands_config,
        &[commands_source],
        &out_dir,
        Path::new("DisplayList/DisplayListCommandsGenerated.h"),
    );

    Ok(())
}
