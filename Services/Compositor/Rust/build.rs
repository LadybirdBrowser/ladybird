/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::env;
use std::error::Error;
use std::path::PathBuf;

fn main() -> Result<(), Box<dyn Error>> {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let out_dir = PathBuf::from(env::var("OUT_DIR")?);

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=cbindgen.toml");

    let mut config = cbindgen::Config::from_file(manifest_dir.join("cbindgen.toml"))?;
    config.namespaces = Some(vec!["Compositor".to_string(), "RustFFI".to_string()]);
    // An ak::Span is laid out as an AK ReadonlySpan, which C++ passes it as.
    for (rust_name, cpp_name) in [
        ("Span", "AK::ReadonlySpan"),
        ("Color", "Gfx::Color"),
        ("FloatPoint", "Gfx::FloatPoint"),
        ("FloatRect", "Gfx::FloatRect"),
        ("IntRect", "Gfx::IntRect"),
        ("IntSize", "Gfx::IntSize"),
        ("ScalingMode", "Gfx::ScalingMode"),
        ("SpatialNodeIndex", "Compositing::SpatialNodeIndex"),
        ("RawAttachment", "IPC::RawAttachment"),
        ("RawMessage", "IPC::FfiRawMessage"),
        ("EncodedValue", "AK::ReadonlyBytes"),
        ("FfiRasterPainter", "Compositing::RustFFI::FfiRasterPainter"),
        ("CanvasId", "Compositing::CanvasId"),
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
            "LibCompositing/DisplayList/DisplayListResourceIds.h",
            "LibGfx/Color.h",
            "LibGfx/Point.h",
            "LibGfx/Rect.h",
            "LibGfx/ScalingMode.h",
            "LibGfx/Size.h",
            "LibIPC/RawAttachment.h",
            "LibCompositing/RustFFI.h",
            "LibIPC/RawMessage.h",
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
    ] {
        println!("cargo:rerun-if-changed={source}");
        builder = builder.with_src(manifest_dir.join(source));
    }
    builder
        .generate()?
        .write_to_file(out_dir.join("CompositorCoreRustFFI.h"));
    Ok(())
}
