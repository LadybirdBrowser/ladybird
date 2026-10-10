/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Property additions to objects compiled code allocated.
//!
//! A `PutById` whose property cache saw the property added to objects of a
//! shape becomes an `AddNamed` when its object is an `AllocateObject` of the
//! graph (such as `this` in an inlined construct): after checking the
//! object's shape and, if the cache entry needs it, that the prototype chain
//! still has nothing the addition would set, it stores the value and gives
//! the object the shape the entry transitions to, like the property cache
//! does. With entries for several shapes (additions, and writes of existing
//! properties), it switches on the object's shape, which check elimination
//! usually knows, so that one case remains. The allocation makes room for the property inline, so the addition
//! never grows the object's storage.
//!
//! Plain objects compiled code allocates can always get properties: they are
//! extensible, and nothing about them makes additions take the slow path.

use super::GraphBuilder;
use super::caches::Action;
use super::caches::Holder;
use crate::CompileFailure;
use crate::bytecode::Operand;
use crate::code::ExitKind;
use crate::ir::Op;

impl GraphBuilder<'_> {
    /// Builds `base.property = src` as `AddNamed` and `StoreNamed` nodes, if
    /// its property cache saw additions (and writes of existing own data
    /// properties) on shapes that are no dictionaries, and its object is an
    /// allocation of the graph that can make room for the properties. With
    /// several shapes, it switches on the object's shape, which is usually
    /// known. Returns false, having built nothing that matters, otherwise.
    pub(super) fn try_build_add_named(
        &mut self,
        base: Operand,
        src: Operand,
        cache: u32,
    ) -> Result<bool, CompileFailure> {
        let Some(entries) = self.cache_entries(cache) else {
            return Ok(false);
        };
        if !self.may_speculate(ExitKind::BadShape) {
            return Ok(false);
        }
        let mut added_count = None;
        for (index, entry) in entries.iter().enumerate() {
            match entry.action {
                Action::Add { offset, .. } => added_count = added_count.max(Some(offset + 1)),
                Action::Set {
                    holder: Holder::Receiver,
                    ..
                } if entry.is_data_access() && entry.shape.dictionary_generation.is_none() => {}
                _ => return Ok(false),
            }
            if entries[..index].iter().any(|other| other.shape == entry.shape) {
                return Ok(false);
            }
        }
        // NB: An addition appends the property to the shape's properties.
        let Some(added_count) = added_count else {
            return Ok(false);
        };
        let object = self.read(base)?;
        let Op::AllocateObject {
            shape: allocated_shape,
            property_count,
            reserve,
        } = self.graph.node(object).op
        else {
            return Ok(false);
        };
        let reserve = reserve.max(added_count);
        if self.runtime.object_allocation.size_class_for(reserve).is_none() {
            return Ok(false);
        }
        let value = self.read(src)?;

        let shapes = entries.iter().map(|entry| entry.shape).collect::<Vec<_>>();
        self.dispatch_on_shapes(
            object,
            &entries,
            &shapes,
            entries.len() == 1,
            |builder, entry, object| {
                match entry.action {
                    Action::Add {
                        offset,
                        shape,
                        prototype_chain,
                    } => {
                        if let Some((validity, valid)) = prototype_chain {
                            builder.check_prototype_chain(validity, valid);
                        }
                        builder.embed(shape);
                        let address = builder.cell_address(object);
                        builder.emit(
                            Op::AddNamed {
                                offset,
                                shape,
                                property_count: offset + 1,
                            },
                            vec![object, value, address],
                            None,
                        );
                    }
                    Action::Set { offset, .. } => builder.store_named(object, offset, value),
                    Action::Get { .. } => unreachable!("additions only add and set"),
                }
                None
            },
        );
        self.graph.nodes[object.index()].op = Op::AllocateObject {
            shape: allocated_shape,
            property_count,
            reserve,
        };
        Ok(true)
    }
}
