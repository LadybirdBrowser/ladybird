/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::env;
use std::error::Error;
use std::path::{Path, PathBuf};
use std::process::Command;

// AK::Optional is not trivial for calls under every C++ ABI: Apple clang returns it through memory, where Rust
// returns the matching #[repr(C)] struct in registers. Keep it out of extern "C" signatures; pass a plain
// #[repr(C)] struct and convert it to an Optional on the C++ side.
fn reject_optional_in_extern_c_signatures(header: &Path) {
    let text = std::fs::read_to_string(header).unwrap_or_default();
    for block in text.split("extern \"C\" {").skip(1) {
        let (signatures, _) = block.split_once("}  // extern \"C\"").unwrap_or((block, ""));
        if let Some(line) = signatures.lines().find(|line| line.contains("Optional<")) {
            panic!(
                "{}: AK::Optional in an extern \"C\" signature: {line}",
                header.display()
            );
        }
    }
}

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
            bindings.write_to_file(&output_header);
            reject_optional_in_extern_c_signatures(&output_header);
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
            bindings.write_to_file(&output_header);
            reject_optional_in_extern_c_signatures(&output_header);
        },
    );
}

fn expose_css_pixel_types_as_web_types(config: &mut cbindgen::Config) {
    for (rust_name, cpp_name) in [
        ("CssPixels", "Web::CSSPixels"),
        ("FfiCssPixelPoint", "Web::CSSPixelPoint"),
        ("FfiCssPixelSize", "Web::CSSPixelSize"),
        ("FfiCssPixelRect", "Web::CSSPixelRect"),
    ] {
        config.export.exclude.push(rust_name.to_string());
        config.export.rename.insert(rust_name.to_string(), cpp_name.to_string());
    }
    config.includes.push("LibWebCommon/PixelUnits.h".to_string());
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
            "DisplayListCommandType",
            "FillRoundedRectRing",
            "DrawIsolatedGroup",
            "ReplayClip",
            "PlayedLayer",
            "PlayedPayload",
            "FilterGraph",
            "ReplayMask",
            "CanvasId",
            "VideoSinkResourceId",
            "Canvas2DPaintKind",
            "Canvas2DPatternRepetition",
            "PathGeometryData",
            "CompositorScrollNodeKind",
            "CompositorScrollNodeRecord",
            "CompositorSnapContainerRecord",
            "CompositorSnapAreaRecord",
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
        ("OptionalCssPixels", "Optional<Web::CSSPixels>"),
        ("OptionalCssPixelRect", "Optional<Web::CSSPixelRect>"),
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
        ("DisplayListCommandType", "Compositing::DisplayListCommandType"),
        ("FillRoundedRectRing", "Compositing::FillRoundedRectRing"),
        ("DrawIsolatedGroup", "Compositing::DrawIsolatedGroup"),
        ("ReplayClip", "Compositing::ReplayClip"),
        ("PlayedLayer", "Compositing::PlayedLayer"),
        ("PlayedPayload", "Compositing::PlayedPayload"),
        ("FilterGraph", "Compositing::FilterGraph"),
        ("ReplayMask", "Compositing::ReplayMask"),
        ("CanvasId", "Compositing::CanvasId"),
        ("VideoSinkResourceId", "Compositing::VideoSinkResourceId"),
        ("Canvas2DPaintKind", "Compositing::Canvas2DPaintKind"),
        ("Canvas2DPatternRepetition", "Compositing::Canvas2DPatternRepetition"),
        ("CapStyle", "Gfx::CapStyle"),
        ("JoinStyle", "Gfx::JoinStyle"),
        ("PathGeometryData", "Gfx::FFI::PathGeometryData"),
        ("CompositorScrollNodeKind", "Compositing::CompositorScrollNodeKind"),
        ("CompositorScrollNodeRecord", "Compositing::CompositorScrollNodeRecord"),
        (
            "CompositorSnapContainerRecord",
            "Compositing::CompositorSnapContainerRecord",
        ),
        ("CompositorSnapAreaRecord", "Compositing::CompositorSnapAreaRecord"),
    ] {
        config.export.rename.insert(rust_name.to_string(), cpp_name.to_string());
    }

    config.includes.extend(
        [
            "AK/Optional.h",
            "LibGfx/AffineTransform.h",
            "LibGfx/Color.h",
            "LibGfx/ColorFilterType.h",
            "LibGfx/CompositingAndBlendingOperator.h",
            "LibGfx/CornerRadii.h",
            "LibGfx/GradientInterpolation.h",
            "LibGfx/InterpolationColorSpace.h",
            "LibGfx/Matrix4x4.h",
            "LibGfx/PathData.h",
            "LibGfx/Point.h",
            "LibGfx/Rect.h",
            "LibGfx/ScalingMode.h",
            "LibGfx/Size.h",
            "LibGfx/StrokeStyle.h",
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

// The commands of the `played_commands!` table that a painter plays.
fn played_commands(path: &Path) -> Result<Vec<String>, Box<dyn Error>> {
    let source = std::fs::read_to_string(path)?;
    let (_, table) = source
        .split_once("\nplayed_commands! {")
        .ok_or("played_commands! table not found")?;
    let (_, played) = table
        .split_once("played: [")
        .ok_or("played_commands! has no played section")?;
    let (played, _) = played.split_once(']').ok_or("unterminated played section")?;
    let names: Vec<String> = played
        .split(',')
        .map(|name| name.trim().to_string())
        .filter(|name| !name.is_empty())
        .collect();
    if let Some(name) = names.iter().find(|name| !name.chars().all(|c| c.is_alphanumeric())) {
        return Err(format!("unparsable played_commands! entry: {name}").into());
    }
    Ok(names)
}

fn generate_ipc_definitions(manifest_dir: &Path, out_dir: &Path) -> Result<(), Box<dyn Error>> {
    let source_dir = manifest_dir.join("../../..");
    let generator = source_dir.join("Meta/Generators/generate_ipc_definitions.py");
    println!("cargo:rerun-if-changed={}", generator.display());
    for module in ["lexer.py", "utils.py"] {
        println!(
            "cargo:rerun-if-changed={}",
            source_dir.join("Meta/Utils").join(module).display()
        );
    }

    println!("cargo:rerun-if-env-changed=PYTHON");
    let python = env::var_os("PYTHON").unwrap_or_else(|| if cfg!(windows) { "python" } else { "python3" }.into());
    for (definition, generated_file) in [
        (
            "Libraries/LibCompositing/DisplayList/DisplayListProperties.ipc",
            "DisplayListProperties.rs",
        ),
        ("Libraries/LibCompositing/Types.ipc", "Types.rs"),
        ("Libraries/LibWebCommon/UIEvents/KeyCode.ipc", "KeyCode.rs"),
        ("Libraries/LibWebCommon/UIEvents/MouseButton.ipc", "MouseButton.rs"),
        (
            "Libraries/LibWebCommon/Page/AsyncScrollNodeStableID.ipc",
            "AsyncScrollNodeStableID.rs",
        ),
        ("Libraries/LibWebCommon/Page/InputEvent.ipc", "InputEvent.rs"),
        (
            "Libraries/LibWebCommon/Page/InputRoutingDecision.ipc",
            "InputRoutingDecision.rs",
        ),
        ("Libraries/LibWebCommon/Page/InputTrace.ipc", "InputTrace.rs"),
        (
            "Libraries/LibWebCommon/Page/TextEditingCommand.ipc",
            "TextEditingCommand.rs",
        ),
        (
            "Services/Compositor/CompositorWebContentClient.ipc",
            "CompositorWebContentClientEndpoint.rs",
        ),
        (
            "Services/Compositor/CompositorWebContentServer.ipc",
            "CompositorWebContentServerEndpoint.rs",
        ),
        (
            "Services/Compositor/CompositorPresentationClient.ipc",
            "CompositorPresentationClientEndpoint.rs",
        ),
        (
            "Services/Compositor/CompositorPresentationServer.ipc",
            "CompositorPresentationServerEndpoint.rs",
        ),
        (
            "Services/Compositor/RenderClockClient.ipc",
            "RenderClockClientEndpoint.rs",
        ),
        (
            "Services/Compositor/RenderClockServer.ipc",
            "RenderClockServerEndpoint.rs",
        ),
    ] {
        let definition = source_dir.join(definition);
        println!("cargo:rerun-if-changed={}", definition.display());
        let status = Command::new(&python)
            .arg(&generator)
            .arg("--input")
            .arg(&definition)
            .arg("--rust-output")
            .arg(out_dir.join(generated_file))
            .status()?;
        if !status.success() {
            return Err(format!("IPC compiler failed for {}", definition.display()).into());
        }
    }
    Ok(())
}

fn main() -> Result<(), Box<dyn Error>> {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let out_dir = PathBuf::from(env::var("OUT_DIR")?);

    generate_ipc_definitions(&manifest_dir, &out_dir)?;

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=cbindgen.toml");
    println!("cargo:rerun-if-changed=src");

    let base_config = cbindgen::Config::from_file(manifest_dir.join("cbindgen.toml"))?;

    // The entry points and the plain values that cross the boundary - namespace Compositing::RustFFI.
    let mut ffi_config = base_config.clone();
    ffi_config.namespaces = Some(vec!["Compositing".to_string(), "RustFFI".to_string()]);
    expose_css_pixel_types_as_web_types(&mut ffi_config);
    expose_shared_abi_types_as_cpp_types(&mut ffi_config);
    ffi_config.export.exclude.extend([
        "MOMENTUM_DISTANCE_SHARE_PER_FRAME".to_string(),
        "MOMENTUM_FRAME_DURATION_IN_SECONDS".to_string(),
        "MAXIMUM_MOMENTUM_DURATION_IN_SECONDS".to_string(),
    ]);
    // An ak::Span is laid out as an AK ReadonlySpan, which C++ passes it as.
    ffi_config
        .export
        .rename
        .insert("Span".to_string(), "AK::ReadonlySpan".to_string());
    ffi_config
        .export
        .rename
        .insert("EncodedValue".to_string(), "AK::ReadonlyBytes".to_string());
    // The IPC compiler checks that these enums have the numbers in C++ that they have in Rust.
    for (rust_name, cpp_name) in [
        ("ScrollAnimationKind", "Compositing::ScrollAnimationKind"),
        ("InputRoutingDecision", "Web::InputRoutingDecision"),
        ("AsyncScrollNodeKind", "Web::AsyncScrollNodeKind"),
    ] {
        ffi_config
            .export
            .rename
            .insert(rust_name.to_string(), cpp_name.to_string());
    }
    ffi_config.after_includes = Some(
        "\nnamespace Compositing { enum class ScrollAnimationKind : uint8_t; }\nnamespace Web { enum class \
         InputRoutingDecision : uint8_t; enum class AsyncScrollNodeKind : uint8_t; }"
            .to_string(),
    );
    ffi_config.includes.push("AK/Span.h".to_string());
    // The values the entry points of WebContent and of the compositor's scroll logic take and return, which their
    // headers refer to here.
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
        "FfiDisplayListReplayCallbacks",
        "FfiDisplayList",
        "FfiEasingDescriptor",
        "FfiEasingKind",
        "FfiLinearEasingPoint",
        "SnapSelectionStrategyType",
    ]
    .map(String::from)
    .to_vec();
    let mut test_builder_config = ffi_config.clone();
    test_builder_config.export.include.clear();
    generate_ffi_header(
        test_builder_config,
        &[manifest_dir.join("src/test_builder.rs")],
        &out_dir,
        Path::new("TestBuilderRustFFI.h"),
    );
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
            manifest_dir.join("src/display_list/canvas_2d.rs"),
            manifest_dir.join("src/display_list/canvas_2d_recorder.rs"),
            manifest_dir.join("src/scrolling/smooth_scroll_animation.rs"),
            manifest_dir.join("src/scrolling/scroll_snap_selection.rs"),
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
    let played_source = manifest_dir.join("src/display_list/played.rs");
    let filter_graph_source = manifest_dir.join("src/display_list/filter_graph.rs");
    let scroll_table_source = manifest_dir.join("src/visual_context/scroll_table.rs");
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
    commands_config.export.include.extend(
        public_type_names(&scroll_table_source)?
            .into_iter()
            .filter(|name| name != "CompositorScrollTable"),
    );
    commands_config
        .export
        .include
        .extend(public_type_names(&played_source)?);
    commands_config
        .export
        .include
        .extend(public_type_names(&filter_graph_source)?);
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
        ("GradientInterpolationMethod", "Gfx::GradientInterpolationMethod"),
        ("GradientInterpolationType", "Gfx::GradientInterpolationMethod::Type"),
        ("RectangularColorSpace", "Gfx::RectangularColorSpace"),
        ("PolarColorSpace", "Gfx::PolarColorSpace"),
        ("HueInterpolationMethod", "Gfx::HueInterpolationMethod"),
        ("InterpolationColorSpace", "Gfx::InterpolationColorSpace"),
        ("WindingRule", "Gfx::WindingRule"),
        ("LineStyle", "Gfx::LineStyle"),
        ("CapStyle", "Gfx::CapStyle"),
        ("JoinStyle", "Gfx::JoinStyle"),
        ("ScalingMode", "Gfx::ScalingMode"),
        ("CompositingAndBlendingOperator", "Gfx::CompositingAndBlendingOperator"),
        ("ColorFilterType", "Gfx::ColorFilterType"),
        ("Orientation", "Gfx::Orientation"),
        ("MaskKind", "Gfx::MaskKind"),
        ("ShouldAntiAlias", "Gfx::ShouldAntiAlias"),
        ("CssPixels", "Web::CSSPixels"),
        ("FfiCssPixelPoint", "Web::CSSPixelPoint"),
        ("FfiCssPixelRect", "Web::CSSPixelRect"),
        ("OptionalFloatRect", "Optional<Gfx::FloatRect>"),
        ("OptionalColor", "Optional<Gfx::Color>"),
        ("OptionalU32", "Optional<u32>"),
        ("OptionalF32", "Optional<float>"),
        ("OptionalAffineTransform", "Optional<Gfx::AffineTransform>"),
        ("CompositorContextId", "Web::CompositorContextId"),
        ("UniqueNodeId", "Web::UniqueNodeID"),
        // An ak::Span is laid out as an AK ReadonlySpan.
        ("Span", "AK::ReadonlySpan"),
        ("PathGeometryData", "Gfx::FFI::PathGeometryData"),
        ("ChannelSelector", "Gfx::FFI::ChannelSelector"),
        ("TurbulenceType", "Gfx::FFI::TurbulenceType"),
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
        "AK/Span.h",
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
        "LibGfx/Point.h",
        "LibGfx/Rect.h",
        "LibGfx/ScalingMode.h",
        "LibGfx/Size.h",
        "LibGfx/StrokeStyle.h",
        "LibGfx/WindingRule.h",
        "LibCompositing/DisplayList/ContextRef.h",
        "LibCompositing/DisplayList/DisplayListResourceIds.h",
        "LibCompositing/Forward.h",
        "LibWebCommon/PixelUnits.h",
        "LibCompositing/Types.h",
    ]
    .into_iter()
    .map(String::from)
    .collect();
    let mut after_includes = String::new();
    let played = played_commands(&played_source)?;
    for (macro_name, canvas_2d) in [
        ("ENUMERATE_PLAYED_DISPLAY_LIST_COMMANDS", false),
        ("ENUMERATE_PLAYED_CANVAS_2D_COMMANDS", true),
    ] {
        after_includes.push_str(&format!("\n#define {macro_name}(V)"));
        for command in played
            .iter()
            .filter(|command| command.starts_with("Canvas2D") == canvas_2d)
        {
            after_includes.push_str(&format!(" \\\n    V({command})"));
        }
        after_includes.push('\n');
    }
    after_includes.push_str(
        "\nnamespace Gfx::FFI {\n\nstruct PathGeometryData;\nenum class ChannelSelector : int32_t;\nenum class \
         TurbulenceType : int32_t;\n\n}\n",
    );
    commands_config.after_includes = Some(after_includes);
    generate_ffi_header_strict(
        commands_config,
        &[commands_source, played_source, filter_graph_source, scroll_table_source],
        &out_dir,
        Path::new("DisplayList/DisplayListCommandsGenerated.h"),
    );

    Ok(())
}
