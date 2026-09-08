/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::sync::{Arc, OnceLock};

struct PathData {
    parsed: crate::svg::ParsedPath,
    serialized: OnceLock<Vec<u16>>,
}

/// Shared parsed path instructions, with serialization produced only when requested.
#[repr(C)]
pub struct CssPath {
    // One owned Arc<PathData>, or zero for non-path shapes.
    raw: usize,
}

impl CssPath {
    pub(crate) fn new(mut parsed: crate::svg::ParsedPath) -> Self {
        // NB: CSS paths previously passed through serialization before geometry consumed
        //     them. Preserve its finite-number clamping without serializing and reparsing.
        parsed.normalize_for_serialization();
        Self {
            raw: Arc::into_raw(Arc::new(PathData {
                parsed,
                serialized: OnceLock::new(),
            })) as usize,
        }
    }

    pub(crate) fn none() -> Self {
        Self { raw: 0 }
    }

    fn data(&self) -> &PathData {
        assert_ne!(self.raw, 0);
        unsafe { &*(self.raw as *const PathData) }
    }

    pub(crate) fn units(&self) -> &[u16] {
        if self.raw == 0 {
            return &[];
        }
        let data = self.data();
        data.serialized.get_or_init(|| data.parsed.serialize_utf16())
    }

    pub(crate) fn to_gfx_path(&self) -> libgfx_rust::path::OwnedPath {
        self.data().parsed.to_gfx_path()
    }
}

impl Clone for CssPath {
    fn clone(&self) -> Self {
        if self.raw != 0 {
            unsafe { Arc::increment_strong_count(self.raw as *const PathData) };
        }
        Self { raw: self.raw }
    }
}

impl Drop for CssPath {
    fn drop(&mut self) {
        if self.raw != 0 {
            unsafe { Arc::decrement_strong_count(self.raw as *const PathData) };
        }
    }
}

impl PartialEq for CssPath {
    fn eq(&self, other: &Self) -> bool {
        self.raw == other.raw || (self.raw != 0 && other.raw != 0 && self.units() == other.units())
    }
}

impl std::fmt::Debug for CssPath {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        self.units().fmt(formatter)
    }
}

/// # Safety
/// `path` must be a live, nonempty CSS path. The caller owns the returned Gfx::Path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_css_path_to_gfx_path(path: &CssPath) -> *mut std::ffi::c_void {
    path.to_gfx_path().into_raw()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shares_parsed_instructions_and_lazy_utf16_serialization() {
        fn assert_send_sync<T: Send + Sync>() {}
        assert_send_sync::<PathData>();
        let source: Vec<_> = "M0 0 L30 40 Z".encode_utf16().collect();
        let path = CssPath::new(crate::svg::parse_utf16_path(&source, false).unwrap());
        let shared = path.clone();
        assert_eq!(path.raw, shared.raw);
        assert!(path.data().serialized.get().is_none());
        assert_eq!(path.data().parsed, shared.data().parsed);
        assert_eq!(path.units(), "M 0 0 L 30 40 Z".encode_utf16().collect::<Vec<_>>());
        assert_eq!(path.units().as_ptr(), shared.units().as_ptr());
        drop(path);
        assert_eq!(shared.units(), "M 0 0 L 30 40 Z".encode_utf16().collect::<Vec<_>>());
    }

    #[test]
    fn preserves_serialized_number_normalization_without_reparsing() {
        let source: Vec<_> = "M1e40 -1e40 C1e40 -0 0 1e40 3 4 A1e40 2 1e40 0 1 5 6"
            .encode_utf16()
            .collect();
        let parsed = crate::svg::parse_utf16_path(&source, false).unwrap();
        let previous = crate::svg::parse_utf16_path(&parsed.serialize_utf16(), false).unwrap();
        let path = CssPath::new(parsed);
        assert_eq!(path.data().parsed, previous);
        assert!(path.data().serialized.get().is_none());
        let shared = path.clone();
        let serialized = std::thread::spawn(move || shared.units().to_vec()).join().unwrap();
        assert_eq!(path.units(), serialized);
    }
}
