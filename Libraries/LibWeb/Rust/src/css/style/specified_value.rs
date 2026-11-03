/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entries elements' own declarations take.

use crate::css::declaration_block;
use crate::css::declaration_block::DeclarationEntry;
use crate::css::style_value::RetainedStyleValueData;
use crate::css::style_value::StyleValueData;

/// The entries of the longhands `declarations` declare, each with the value an element declaring
/// it is written with, a shorthand's longhands taking its value.
pub(crate) fn element_declared_properties<'a>(
    declarations: impl ExactSizeIterator<Item = declaration_block::DeclarationRef<'a>>,
) -> Vec<(DeclarationEntry, RetainedStyleValueData)> {
    fn append(
        property: u16,
        declaration: declaration_block::DeclarationRef<'_>,
        declarations: &mut Vec<(DeclarationEntry, RetainedStyleValueData)>,
    ) {
        use crate::css::property_metadata::{longhands_for_shorthand, property_is_shorthand};
        // Element declarations decide longhands. Presentation attributes can name shorthands
        // whose children are themselves shorthands, so expand the complete property inventory.
        // The longhands of a shorthand awaiting substitution have declarations of their own, which
        // a later change to the block may have replaced or removed.
        if property_is_shorthand(property)
            && !matches!(
                &**declaration.value,
                StyleValueData::Unresolved { .. } | StyleValueData::PendingSubstitution { .. }
            )
        {
            for &longhand in longhands_for_shorthand(property) {
                append(longhand, declaration, declarations);
            }
        } else {
            declarations.push((
                DeclarationEntry::new(property, declaration.important, declaration.operator),
                RetainedStyleValueData::from_arc(declaration.value.clone()),
            ));
        }
    }
    let mut entries = Vec::with_capacity(declarations.len());
    for declaration in declarations {
        append(declaration.property_id, declaration, &mut entries);
    }
    entries
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::declaration_block::DeclarationBlockBuilder;
    use crate::css::declaration_block::DeclaredProperty;
    use crate::css::property_metadata::property_id;
    use crate::css::style_value::shared_style_value;

    #[test]
    fn a_shorthand_hint_declares_each_longhand_with_one_value() {
        let mut hints = DeclarationBlockBuilder::default();
        hints.append_in_specified_order(DeclaredProperty {
            property_id: property_id::BORDER,
            important: false,
            value: shared_style_value(StyleValueData::Keyword {
                keyword: crate::css::style_compute::keyword::INHERIT,
            }),
        });
        let declared = element_declared_properties(hints.properties.iter().map(DeclaredProperty::borrowed));
        // The longhands comprise four widths, four styles, four colors, and five border-image properties.
        assert_eq!(declared.len(), 17);
        assert!(
            declared
                .iter()
                .all(|(_, written)| written.pointer() == declared[0].1.pointer())
        );
    }
}
