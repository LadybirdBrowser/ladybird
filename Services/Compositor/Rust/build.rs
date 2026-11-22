/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::env;
use std::error::Error;
use std::path::{Path, PathBuf};

#[allow(dead_code, reason = "the model is for the generators of the other crates as well")]
#[path = "../../../Libraries/LibCompositing/Rust/generate/webgl_wire_model.rs"]
mod webgl_wire_model;

#[path = "generate/webgl_replayer.rs"]
mod webgl_replayer;

/// The GL functions that the replayer of WebGL calls, which src/webgl/gl.rs includes, and the methods of the replayer,
/// which src/webgl/replay.rs includes.
fn generate_webgl_replayer(manifest_dir: &Path, out_dir: &Path) -> Result<(), Box<dyn Error>> {
    let libraries = manifest_dir.join("../../../Libraries");
    let functions_path = libraries.join("LibCompositing/WebGL/GLFunctions.json");
    println!("cargo:rerun-if-changed={}", functions_path.display());
    println!(
        "cargo:rerun-if-changed={}",
        libraries.join("LibCompositing/Rust/generate").display()
    );
    println!("cargo:rerun-if-changed=generate");
    let json = std::fs::read_to_string(&functions_path)?;
    let generate = |json: &str| -> Result<(String, String), String> {
        let functions = webgl_wire_model::parse_functions(json)?;
        Ok((
            webgl_replayer::write_gl_functions(&functions)?,
            webgl_replayer::write_replayer(&functions)?,
        ))
    };
    let (gl_functions, replayer) = generate(&json).map_err(|error| format!("{}: {error}", functions_path.display()))?;
    std::fs::write(out_dir.join("webgl_gl.rs"), gl_functions)?;
    std::fs::write(out_dir.join("webgl_replayer.rs"), replayer)?;
    Ok(())
}

fn main() -> Result<(), Box<dyn Error>> {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let out_dir = PathBuf::from(env::var("OUT_DIR")?);

    generate_webgl_replayer(&manifest_dir, &out_dir)?;

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=cbindgen.toml");

    let mut config = cbindgen::Config::from_file(manifest_dir.join("cbindgen.toml"))?;
    config.namespaces = Some(vec!["Compositor".to_string(), "RustFFI".to_string()]);
    // An ak::Span is laid out as an AK ReadonlySpan, which C++ passes it as.
    for (rust_name, cpp_name) in [
        ("Span", "AK::ReadonlySpan"),
        ("Color", "Gfx::Color"),
        ("CornerRadii", "Gfx::CornerRadii"),
        ("FloatPoint", "Gfx::FloatPoint"),
        ("FloatRect", "Gfx::FloatRect"),
        ("IntRect", "Gfx::IntRect"),
        ("IntSize", "Gfx::IntSize"),
        ("ScalingMode", "Gfx::ScalingMode"),
        ("WindingRule", "Gfx::WindingRule"),
        ("PathGeometryData", "Gfx::FFI::PathGeometryData"),
        ("SpatialNodeIndex", "Compositing::SpatialNodeIndex"),
        ("RawAttachment", "IPC::RawAttachment"),
        ("RawMessage", "IPC::FfiRawMessage"),
        ("EncodedValue", "AK::ReadonlyBytes"),
        ("FfiRasterPainter", "Compositing::RustFFI::FfiRasterPainter"),
        ("CanvasId", "Compositing::CanvasId"),
        ("DisplayListCommandType", "Compositing::DisplayListCommandType"),
        (
            "Web_Bindings_PredefinedColorSpace",
            "Web::Bindings::PredefinedColorSpace",
        ),
        ("ImageFrameResourceId", "Compositing::ImageFrameResourceId"),
        ("PaintingSurface", "Gfx::PaintingSurface"),
        ("PresentableSurface", "Compositor::PresentableSurface"),
        ("WeakPresentableSurface", "Compositor::FfiWeakPresentableSurface"),
        ("PresentableSurfacePool", "Compositor::FfiPresentableSurfacePool"),
    ] {
        config.export.rename.insert(rust_name.to_string(), cpp_name.to_string());
    }
    // The surfaces are the C++ classes that stand for them, and the C++ declares the rest, so they are only named.
    config.export.exclude.extend(
        [
            "PaintingSurface",
            "PresentableSurface",
            "WeakPresentableSurface",
            "PresentableSurfacePool",
        ]
        .map(String::from),
    );
    config.includes.extend(
        [
            "AK/Span.h",
            "Compositor/PaintingSurface.h",
            "Compositor/PresentableSurface.h",
            "LibCompositing/DisplayList/ContextRef.h",
            "LibCompositing/DisplayList/DisplayListCommandsGenerated.h",
            "LibCompositing/DisplayList/DisplayListResourceIds.h",
            "LibGfx/Color.h",
            "LibGfx/CornerRadii.h",
            "LibGfx/Point.h",
            "LibGfx/Rect.h",
            "LibGfx/RustFFI.h",
            "LibGfx/ScalingMode.h",
            "LibGfx/Size.h",
            "LibGfx/WindingRule.h",
            "LibIPC/RawAttachment.h",
            "LibCompositing/RustFFI.h",
            "LibIPC/RawMessage.h",
            "LibWebCommon/Bindings/PredefinedColorSpace.h",
        ]
        .map(String::from),
    );

    let mut builder = cbindgen::Builder::new().with_config(config);
    for source in [
        "src/ffi.rs",
        "src/host.rs",
        "src/frame_inputs.rs",
        "src/raster_host.rs",
        "src/core/presenting.rs",
        "src/core/mod.rs",
        "src/backing_stores.rs",
        "src/cpp_object.rs",
        "src/surfaces/painting_surface.rs",
        "src/surfaces/presentable_surface.rs",
        "src/webgl/host.rs",
    ] {
        println!("cargo:rerun-if-changed={source}");
        builder = builder.with_src(manifest_dir.join(source));
    }
    builder
        .generate()?
        .write_to_file(out_dir.join("CompositorCoreRustFFI.h"));
    Ok(())
}
