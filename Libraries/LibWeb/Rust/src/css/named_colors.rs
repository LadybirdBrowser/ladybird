/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

include!(concat!(env!("OUT_DIR"), "/named_colors_generated.rs"));

#[cfg(test)]
mod tests {
    use super::*;

    fn lookup(name: &str) -> Option<[u8; 4]> {
        named_color_from_name(name.encode_utf16().collect::<Vec<_>>().as_slice().into())
    }

    #[test]
    fn looks_up_named_colors_case_insensitively() {
        assert_eq!(lookup("ReD"), Some([0xff, 0x00, 0x00, 0xff]));
        assert_eq!(lookup("rebeccapurple"), Some([0x66, 0x33, 0x99, 0xff]));
        assert_eq!(lookup("transparent"), None);
        assert_eq!(lookup("unknown"), None);
    }
}
