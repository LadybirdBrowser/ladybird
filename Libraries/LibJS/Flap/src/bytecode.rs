/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Prepared bytecode metadata used by reusable handler lowering.

use bytecode_def::OpLayout;

/// The identity of a handler parameter that names a bytecode field.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) struct BytecodeFieldId(u32);

impl BytecodeFieldId {
    pub(crate) fn new(index: usize) -> Self {
        Self(
            u32::try_from(index)
                .expect("Flap handler parameter count fits in u32"),
        )
    }

    pub(crate) fn index(self) -> usize {
        self.0 as usize
    }
}

#[derive(Debug)]
struct FieldLayout {
    name: String,
    offset: Option<usize>,
}

/// Bytecode layout data aligned with one prepared handler's parameters.
#[derive(Debug)]
pub(crate) struct HandlerLayout {
    pub(crate) size: Option<usize>,
    fields: Vec<FieldLayout>,
}

impl HandlerLayout {
    pub(crate) fn new(
        parameter_names: &[String],
        layout: Option<&OpLayout>,
    ) -> Self {
        Self {
            size: layout.and_then(|layout| layout.size),
            fields: parameter_names
                .iter()
                .map(|name| {
                    let name = bytecode_field_name(name);
                    FieldLayout {
                        offset: layout
                            .and_then(|layout| layout.field_offsets.get(&name))
                            .copied(),
                        name,
                    }
                })
                .collect(),
        }
    }

    pub(crate) fn field_name(&self, field: BytecodeFieldId) -> &str {
        &self.fields[field.index()].name
    }

    pub(crate) fn field_offset(
        &self,
        field: BytecodeFieldId,
    ) -> Option<usize> {
        self.fields[field.index()].offset
    }
}

fn bytecode_field_name(name: &str) -> String {
    name.strip_prefix("m_")
        .map_or_else(|| format!("m_{name}"), |_| name.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    #[test]
    fn aligns_layout_offsets_with_handler_parameters() {
        let layout = OpLayout {
            field_offsets: HashMap::from([
                ("m_lhs".to_string(), 4),
                ("m_rhs".to_string(), 8),
            ]),
            size: Some(12),
        };
        let prepared = HandlerLayout::new(
            &["lhs".to_string(), "m_rhs".to_string()],
            Some(&layout),
        );

        assert_eq!(prepared.size, Some(12));
        assert_eq!(
            prepared.field_offset(BytecodeFieldId::new(0)),
            Some(4)
        );
        assert_eq!(
            prepared.field_offset(BytecodeFieldId::new(1)),
            Some(8)
        );
        assert_eq!(
            prepared.field_name(BytecodeFieldId::new(1)),
            "m_rhs"
        );
    }
}
