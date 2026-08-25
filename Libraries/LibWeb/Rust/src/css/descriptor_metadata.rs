/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

include!(concat!(env!("OUT_DIR"), "/descriptor_metadata_generated.rs"));

#[cfg(test)]
mod tests {
    use super::{
        CUSTOM_DESCRIPTOR_ID, DescriptorSyntax, DescriptorValueType, descriptor_longhands, descriptor_metadata,
    };
    use crate::css::property_metadata::property_id;

    #[test]
    fn generates_descriptor_names_aliases_and_syntax() {
        let font_width = descriptor_metadata(0, &"font-width".encode_utf16().collect::<Vec<_>>()).unwrap();
        let font_stretch = descriptor_metadata(0, &"FoNt-StReTcH".encode_utf16().collect::<Vec<_>>()).unwrap();
        assert_eq!(font_stretch.id, font_width.id);
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
        assert_eq!(custom.id, CUSTOM_DESCRIPTOR_ID);
        assert!(custom.allow_arbitrary_substitution_functions);
        assert!(custom.allow_css_wide_keywords);
        assert!(descriptor_metadata(1, &"unknown".encode_utf16().collect::<Vec<_>>()).is_none());

        let margin = descriptor_metadata(1, &"margin".encode_utf16().collect::<Vec<_>>()).unwrap();
        let longhands = descriptor_longhands(1, margin.id);
        assert_eq!(longhands.len(), 4);
        assert_eq!(longhands[0].property_id, property_id::MARGIN_TOP);
        assert_eq!(longhands[1].property_id, property_id::MARGIN_RIGHT);
        assert_eq!(longhands[2].property_id, property_id::MARGIN_BOTTOM);
        assert_eq!(longhands[3].property_id, property_id::MARGIN_LEFT);
    }
}
