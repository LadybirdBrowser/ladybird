/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

include!(concat!(env!("OUT_DIR"), "/descriptor_metadata_generated.rs"));

#[cfg(test)]
mod tests {
    use super::{DescriptorSyntax, DescriptorValueType, descriptor_metadata};

    #[test]
    fn generates_descriptor_names_aliases_and_syntax() {
        let font_width = descriptor_metadata(0, &"font-width".encode_utf16().collect::<Vec<_>>()).unwrap();
        let font_stretch = descriptor_metadata(0, &"FoNt-StReTcH".encode_utf16().collect::<Vec<_>>()).unwrap();
        assert_eq!(font_stretch.syntax, font_width.syntax);
        assert_eq!(
            font_stretch.allow_arbitrary_substitution_functions,
            font_width.allow_arbitrary_substitution_functions
        );
        assert_eq!(font_stretch.allow_css_wide_keywords, font_width.allow_css_wide_keywords);
        assert!(matches!(
            font_width.syntax,
            [DescriptorSyntax::Keyword("auto"), DescriptorSyntax::Property(_)]
        ));

        let source = descriptor_metadata(0, &"src".encode_utf16().collect::<Vec<_>>()).unwrap();
        assert_eq!(
            source.syntax,
            [DescriptorSyntax::ValueType(DescriptorValueType::FontSrcList)]
        );

        let custom = descriptor_metadata(4, &"--result".encode_utf16().collect::<Vec<_>>()).unwrap();
        assert!(custom.allow_arbitrary_substitution_functions);
        assert!(custom.allow_css_wide_keywords);
        assert!(descriptor_metadata(1, &"unknown".encode_utf16().collect::<Vec<_>>()).is_none());
    }
}
