/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://drafts.fxtf.org/geometry/#domquad

use libjs_rust::api::{Trace, Vm};

use super::dictionaries::DOMPointInit;
pub(crate) use super::dictionaries::DOMQuadInit;
use super::dictionaries::DOMRectInit;
use super::dom_point::DOMPoint;
use super::dom_rect::DOMRect;
use super::{nan_safe_maximum, nan_safe_minimum};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::structured_data::{Serializable, SerializationReader, SerializationWriter};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DOMQuad {
    p1: Ref<DOMPoint>,
    p2: Ref<DOMPoint>,
    p3: Ref<DOMPoint>,
    p4: Ref<DOMPoint>,
}

impl Wrappable for DOMQuad {}

/// A new DOMPoint object with its attributes set to the values of the namesake dictionary members in the dictionary.
fn new_point(vm: &Vm, init: &DOMPointInit) -> Ref<DOMPoint> {
    allocate(vm, |_| DOMPoint::from_dictionary(init))
}

impl DOMQuad {
    /// https://drafts.fxtf.org/geometry/#dom-domquad-domquad
    pub(crate) fn construct(
        cx: &mut CallContext,
        p1: DOMPointInit,
        p2: DOMPointInit,
        p3: DOMPointInit,
        p4: DOMPointInit,
    ) -> WebIdlResult<Self> {
        let vm = cx.vm();

        // 1. Let point1 be a new DOMPoint object with its attributes set to the values of the namesake dictionary
        //    members in p1.
        let point1 = new_point(vm, &p1);

        // 2. Let point2 be a new DOMPoint object with its attributes set to the values of the namesake dictionary
        //    members in p2.
        let point2 = new_point(vm, &p2);

        // 3. Let point3 be a new DOMPoint object with its attributes set to the values of the namesake dictionary
        //    members in p3.
        let point3 = new_point(vm, &p3);

        // 4. Let point4 be a new DOMPoint object with its attributes set to the values of the namesake dictionary
        //    members in p4.
        let point4 = new_point(vm, &p4);

        // 5. Return a new DOMQuad with point 1 set to point1, point 2 set to point2, point 3 set to point3 and point 4
        //    set to point4.
        Ok(Self {
            p1: point1,
            p2: point2,
            p3: point3,
            p4: point4,
        })
    }

    /// https://drafts.fxtf.org/geometry/#dom-domquad-fromrect
    pub(crate) fn from_rect(cx: &mut CallContext, other: DOMRectInit) -> WebIdlResult<Ref<Self>> {
        // The fromRect(other) static method on DOMQuad must create a DOMQuad from the DOMRectInit dictionary other.
        let quad = Self::from_rect_dictionary(cx.vm(), &other);
        Ok(allocate(cx.vm(), |_| quad))
    }

    /// https://drafts.fxtf.org/geometry/#create-a-domquad-from-the-domrectinit-dictionary
    fn from_rect_dictionary(vm: &Vm, other: &DOMRectInit) -> Self {
        // 1. Let x, y, width and height be the value of other’s x, y, width and height dictionary members,
        //    respectively.
        let DOMRectInit { x, y, width, height } = *other;

        // 2. Let point1 be a new DOMPoint object with x coordinate set to x, y coordinate set to y, z coordinate set
        //    to 0 and w perspective set to 1.
        let point1 = allocate(vm, |_| DOMPoint::new(x, y, 0.0, 1.0));

        // 3. Let point2 be a new DOMPoint object with x coordinate set to x + width, y coordinate set to y, z
        //    coordinate set to 0 and w perspective set to 1.
        let point2 = allocate(vm, |_| DOMPoint::new(x + width, y, 0.0, 1.0));

        // 4. Let point3 be a new DOMPoint object with x coordinate set to x + width, y coordinate set to y + height, z
        //    coordinate set to 0 and w perspective set to 1.
        let point3 = allocate(vm, |_| DOMPoint::new(x + width, y + height, 0.0, 1.0));

        // 5. Let point4 be a new DOMPoint object with x coordinate set to x, y coordinate set to y + height, z
        //    coordinate set to 0 and w perspective set to 1.
        let point4 = allocate(vm, |_| DOMPoint::new(x, y + height, 0.0, 1.0));

        // 6. Return a new DOMQuad with point 1 set to point1, point 2 set to point2, point 3 set to point3 and point 4
        //    set to point4.
        Self {
            p1: point1,
            p2: point2,
            p3: point3,
            p4: point4,
        }
    }

    /// https://drafts.fxtf.org/geometry/#dom-domquad-fromquad
    pub(crate) fn from_quad(cx: &mut CallContext, other: DOMQuadInit) -> WebIdlResult<Ref<Self>> {
        // The fromQuad(other) static method on DOMQuad must create a DOMQuad from the DOMQuadInit dictionary other.
        let vm = cx.vm();

        // https://drafts.fxtf.org/geometry/#create-a-domquad-from-the-domquadinit-dictionary
        // 1. Let point1 be the result of invoking create a DOMPoint from the dictionary p1 dictionary member of other,
        //    if it exists.
        let point1 = new_point(vm, &other.p1.unwrap_or_default());

        // 2. Let point2 be the result of invoking create a DOMPoint from the dictionary p2 dictionary member of other,
        //    if it exists.
        let point2 = new_point(vm, &other.p2.unwrap_or_default());

        // 3. Let point3 be the result of invoking create a DOMPoint from the dictionary p3 dictionary member of other,
        //    if it exists.
        let point3 = new_point(vm, &other.p3.unwrap_or_default());

        // 4. Let point4 be the result of invoking create a DOMPoint from the dictionary p4 dictionary member of other,
        //    if it exists.
        let point4 = new_point(vm, &other.p4.unwrap_or_default());

        // 5. Return a new DOMQuad with point 1 set to point1, point 2 set to point2, point 3 set to point3 and point 4
        //    set to point4.
        Ok(allocate(vm, |_| Self {
            p1: point1,
            p2: point2,
            p3: point3,
            p4: point4,
        }))
    }

    /// https://drafts.fxtf.org/geometry/#dom-domquad-p1
    pub(crate) fn p1(&self, _: &mut CallContext) -> WebIdlResult<Ref<DOMPoint>> {
        // The p1 attribute must return point 1.
        Ok(self.p1)
    }

    /// https://drafts.fxtf.org/geometry/#dom-domquad-p2
    pub(crate) fn p2(&self, _: &mut CallContext) -> WebIdlResult<Ref<DOMPoint>> {
        // The p2 attribute must return point 2.
        Ok(self.p2)
    }

    /// https://drafts.fxtf.org/geometry/#dom-domquad-p3
    pub(crate) fn p3(&self, _: &mut CallContext) -> WebIdlResult<Ref<DOMPoint>> {
        // The p3 attribute must return point 3.
        Ok(self.p3)
    }

    /// https://drafts.fxtf.org/geometry/#dom-domquad-p4
    pub(crate) fn p4(&self, _: &mut CallContext) -> WebIdlResult<Ref<DOMPoint>> {
        // The p4 attribute must return point 4.
        Ok(self.p4)
    }

    /// https://drafts.fxtf.org/geometry/#dom-domquad-getbounds
    pub(crate) fn get_bounds(&self, cx: &mut CallContext) -> WebIdlResult<Ref<DOMRect>> {
        let [x1, y1, ..] = self.p1.parent().coordinates();
        let [x2, y2, ..] = self.p2.parent().coordinates();
        let [x3, y3, ..] = self.p3.parent().coordinates();
        let [x4, y4, ..] = self.p4.parent().coordinates();

        // 1. Let bounds be a DOMRect object.
        // 2. Let left be the NaN-safe minimum of point 1’s x coordinate, point 2’s x coordinate, point 3’s x
        //    coordinate and point 4’s x coordinate.
        let left = nan_safe_minimum(&[x1, x2, x3, x4]);

        // 3. Let top be the NaN-safe minimum of point 1’s y coordinate, point 2’s y coordinate, point 3’s y coordinate
        //    and point 4’s y coordinate.
        let top = nan_safe_minimum(&[y1, y2, y3, y4]);

        // 4. Let right be the NaN-safe maximum of point 1’s x coordinate, point 2’s x coordinate, point 3’s x
        //    coordinate and point 4’s x coordinate.
        let right = nan_safe_maximum(&[x1, x2, x3, x4]);

        // 5. Let bottom be the NaN-safe maximum of point 1’s y coordinate, point 2’s y coordinate, point 3’s y
        //    coordinate and point 4’s y coordinate.
        let bottom = nan_safe_maximum(&[y1, y2, y3, y4]);

        // 6. Set x coordinate of bounds to left, y coordinate of bounds to top, width dimension of bounds to right -
        //    left and height dimension of bounds to bottom - top.
        // 7. Return bounds.
        Ok(allocate(cx.vm(), |_| {
            DOMRect::new(left, top, right - left, bottom - top)
        }))
    }
}

impl Serializable for DOMQuad {
    /// https://drafts.fxtf.org/geometry/#structured-serialization
    fn serialization_steps(
        &self,
        cx: &mut CallContext,
        serialized: &mut SerializationWriter,
        for_storage: bool,
    ) -> WebIdlResult<()> {
        // NB: The points are serialized in place, as they were before DOMQuad was implemented in Rust, so that stored
        //     values keep the positions that their objects have in the memory of the serialization.

        // 1. Set serialized.[[P1]] to the sub-serialization of value’s point 1.
        serialized.write_object_in_place::<DOMPoint>(cx, &self.p1, for_storage)?;

        // 2. Set serialized.[[P2]] to the sub-serialization of value’s point 2.
        serialized.write_object_in_place::<DOMPoint>(cx, &self.p2, for_storage)?;

        // 3. Set serialized.[[P3]] to the sub-serialization of value’s point 3.
        serialized.write_object_in_place::<DOMPoint>(cx, &self.p3, for_storage)?;

        // 4. Set serialized.[[P4]] to the sub-serialization of value’s point 4.
        serialized.write_object_in_place::<DOMPoint>(cx, &self.p4, for_storage)?;

        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#structured-serialization
    fn deserialization_steps(cx: &mut CallContext, serialized: &mut SerializationReader) -> WebIdlResult<Self> {
        // 1. Set value’s point 1 to the sub-deserialization of serialized.[[P1]].
        let p1 = serialized.read_object_in_place::<DOMPoint>(cx)?;
        let p1 = allocate(cx.vm(), |_| p1);

        // 2. Set value’s point 2 to the sub-deserialization of serialized.[[P2]].
        let p2 = serialized.read_object_in_place::<DOMPoint>(cx)?;
        let p2 = allocate(cx.vm(), |_| p2);

        // 3. Set value’s point 3 to the sub-deserialization of serialized.[[P3]].
        let p3 = serialized.read_object_in_place::<DOMPoint>(cx)?;
        let p3 = allocate(cx.vm(), |_| p3);

        // 4. Set value’s point 4 to the sub-deserialization of serialized.[[P4]].
        let p4 = serialized.read_object_in_place::<DOMPoint>(cx)?;
        let p4 = allocate(cx.vm(), |_| p4);

        Ok(Self { p1, p2, p3, p4 })
    }
}
