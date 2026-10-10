/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The code buffer shared by both assemblers: bytes, labels and fixups.

/// Why assembling failed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AsmError {
    /// A label was referenced but never bound.
    UnboundLabel,
    /// A branch or PC-relative reference does not reach its label.
    BranchOutOfRange,
}

/// A position in the code, bound once and referenced any number of times
/// before or after binding.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Label(u32);

/// How a reference to a label is patched once the label is bound.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FixupKind {
    /// x86-64 rel8 at the fixup offset, relative to the end of the byte.
    X86Rel8,
    /// x86-64 rel32 at the fixup offset, relative to the end of the field.
    X86Rel32,
    /// AArch64 B/BL: imm26 word offset in bits 0..26.
    A64Branch26,
    /// AArch64 B.cond/CBZ/CBNZ: imm19 word offset in bits 5..24.
    A64Branch19,
    /// AArch64 TBZ/TBNZ: imm14 word offset in bits 5..19.
    A64Branch14,
    /// AArch64 ADR: 21-bit byte offset split into immlo (29..31) and immhi (5..24).
    A64Adr21,
}

impl FixupKind {
    /// The largest forward distance in bytes this kind can encode.
    pub const fn max_forward_distance(self) -> i64 {
        match self {
            FixupKind::X86Rel8 => i8::MAX as i64,
            FixupKind::X86Rel32 => i32::MAX as i64,
            FixupKind::A64Branch26 => ((1 << 25) - 1) * 4,
            FixupKind::A64Branch19 => ((1 << 18) - 1) * 4,
            FixupKind::A64Branch14 => ((1 << 13) - 1) * 4,
            FixupKind::A64Adr21 => (1 << 20) - 1,
        }
    }

    /// The largest backward distance in bytes this kind can encode.
    pub const fn max_backward_distance(self) -> i64 {
        match self {
            FixupKind::X86Rel8 => 128,
            FixupKind::X86Rel32 => 1 << 31,
            FixupKind::A64Branch26 => (1 << 25) * 4,
            FixupKind::A64Branch19 => (1 << 18) * 4,
            FixupKind::A64Branch14 => (1 << 13) * 4,
            FixupKind::A64Adr21 => 1 << 20,
        }
    }

    /// Whether a reference of this kind starting at `from` reaches `to`.
    pub const fn reaches(self, from: usize, to: usize) -> bool {
        let distance = to as i64 - self.origin(from) as i64;
        distance <= self.max_forward_distance() && -distance <= self.max_backward_distance()
    }

    /// The position the displacement is relative to, for a fixup at `at`.
    const fn origin(self, at: usize) -> usize {
        match self {
            FixupKind::X86Rel8 => at + 1,
            FixupKind::X86Rel32 => at + 4,
            _ => at,
        }
    }
}

const NO_FIXUP: u32 = u32::MAX;

#[derive(Debug, Clone, Copy)]
enum LabelState {
    /// Not bound yet; the head of the list of fixups referencing it.
    Unbound {
        first_fixup: u32,
    },
    Bound(u32),
}

#[derive(Debug, Clone, Copy)]
struct Fixup {
    at: u32,
    kind: FixupKind,
    label: Label,
    /// Next fixup referencing the same label.
    next: u32,
    /// Resolved fixups stay in the list but are skipped.
    resolved: bool,
}

/// A pending fixup, as seen by an assembler that manages branch veneers.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PendingFixup {
    pub id: u32,
    pub at: usize,
    pub kind: FixupKind,
    pub label: Label,
}

/// A growable code buffer with labels.
#[derive(Debug, Default)]
pub struct CodeBuffer {
    bytes: Vec<u8>,
    labels: Vec<LabelState>,
    fixups: Vec<Fixup>,
    /// The first error hit while emitting; reported by `finish()`.
    error: Option<AsmError>,
}

impl CodeBuffer {
    pub fn new() -> Self {
        Self::default()
    }

    /// The current emission offset.
    pub fn offset(&self) -> usize {
        self.bytes.len()
    }

    pub fn emit_u8(&mut self, value: u8) {
        self.bytes.push(value);
    }

    pub fn emit_u16(&mut self, value: u16) {
        self.bytes.extend_from_slice(&value.to_le_bytes());
    }

    pub fn emit_u32(&mut self, value: u32) {
        self.bytes.extend_from_slice(&value.to_le_bytes());
    }

    pub fn emit_u64(&mut self, value: u64) {
        self.bytes.extend_from_slice(&value.to_le_bytes());
    }

    pub fn emit_bytes(&mut self, bytes: &[u8]) {
        self.bytes.extend_from_slice(bytes);
    }

    pub fn read_u32(&self, at: usize) -> u32 {
        u32::from_le_bytes(self.bytes[at..at + 4].try_into().unwrap())
    }

    pub fn write_u32(&mut self, at: usize, value: u32) {
        self.bytes[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }

    /// Records an error to be reported by `finish()`. The first one wins.
    pub fn set_error(&mut self, error: AsmError) {
        self.error.get_or_insert(error);
    }

    pub fn new_label(&mut self) -> Label {
        let label = Label(u32::try_from(self.labels.len()).expect("too many labels"));
        self.labels.push(LabelState::Unbound { first_fixup: NO_FIXUP });
        label
    }

    /// The offset a label is bound to, if it is bound.
    pub fn label_offset(&self, label: Label) -> Option<usize> {
        match self.labels[label.0 as usize] {
            LabelState::Bound(offset) => Some(offset as usize),
            LabelState::Unbound { .. } => None,
        }
    }

    /// Binds `label` to the current offset and patches every reference to it.
    pub fn bind(&mut self, label: Label) {
        let offset = self.offset();
        let state = &mut self.labels[label.0 as usize];
        let LabelState::Unbound { first_fixup } = *state else {
            panic!("label bound twice");
        };
        *state = LabelState::Bound(Self::offset_u32(offset));
        let mut fixup_id = first_fixup;
        while fixup_id != NO_FIXUP {
            let fixup = self.fixups[fixup_id as usize];
            if !fixup.resolved {
                self.patch(fixup.at as usize, fixup.kind, offset);
                self.fixups[fixup_id as usize].resolved = true;
            }
            fixup_id = fixup.next;
        }
    }

    /// Records a reference of `kind` at `at` to `label`. If the label is
    /// already bound, the reference is patched immediately. Returns the
    /// fixup's id if it is still pending.
    pub fn add_fixup(&mut self, label: Label, at: usize, kind: FixupKind) -> Option<u32> {
        match self.labels[label.0 as usize] {
            LabelState::Bound(target) => {
                self.patch(at, kind, target as usize);
                None
            }
            LabelState::Unbound { first_fixup } => {
                let id = u32::try_from(self.fixups.len()).expect("too many fixups");
                self.fixups.push(Fixup {
                    at: Self::offset_u32(at),
                    kind,
                    label,
                    next: first_fixup,
                    resolved: false,
                });
                self.labels[label.0 as usize] = LabelState::Unbound { first_fixup: id };
                Some(id)
            }
        }
    }

    /// The fixup with `id`, if it is still pending.
    pub fn pending_fixup(&self, id: u32) -> Option<PendingFixup> {
        let fixup = &self.fixups[id as usize];
        (!fixup.resolved).then_some(PendingFixup {
            id,
            at: fixup.at as usize,
            kind: fixup.kind,
            label: fixup.label,
        })
    }

    /// Patches a pending fixup to point at `target` instead of its label (a
    /// veneer that will itself branch to the label).
    pub fn redirect_fixup(&mut self, id: u32, target: usize) {
        let fixup = self.fixups[id as usize];
        assert!(!fixup.resolved, "redirecting a resolved fixup");
        self.patch(fixup.at as usize, fixup.kind, target);
        self.fixups[id as usize].resolved = true;
    }

    /// Pads with `filler` until the offset is a multiple of `alignment`.
    pub fn align_with(&mut self, alignment: usize, filler: u8) {
        assert!(alignment.is_power_of_two());
        while !self.offset().is_multiple_of(alignment) {
            self.emit_u8(filler);
        }
    }

    /// Returns the code, or the first error. Fails if a referenced label was
    /// never bound.
    pub fn finish(self) -> Result<Vec<u8>, AsmError> {
        if let Some(error) = self.error {
            return Err(error);
        }
        if self.fixups.iter().any(|fixup| !fixup.resolved) {
            return Err(AsmError::UnboundLabel);
        }
        Ok(self.bytes)
    }

    fn offset_u32(offset: usize) -> u32 {
        u32::try_from(offset).expect("code buffer exceeds 4 GiB")
    }

    fn patch(&mut self, at: usize, kind: FixupKind, target: usize) {
        if !kind.reaches(at, target) {
            self.set_error(AsmError::BranchOutOfRange);
            return;
        }
        let distance = target as i64 - kind.origin(at) as i64;
        match kind {
            FixupKind::X86Rel8 => self.bytes[at] = distance as i8 as u8,
            FixupKind::X86Rel32 => {
                self.bytes[at..at + 4].copy_from_slice(&(distance as i32).to_le_bytes());
            }
            FixupKind::A64Branch26 => {
                let word = self.read_u32(at) & !0x03ff_ffff;
                self.write_u32(at, word | ((distance >> 2) as u32 & 0x03ff_ffff));
            }
            FixupKind::A64Branch19 => {
                let word = self.read_u32(at) & !(0x7ffff << 5);
                self.write_u32(at, word | (((distance >> 2) as u32 & 0x7ffff) << 5));
            }
            FixupKind::A64Branch14 => {
                let word = self.read_u32(at) & !(0x3fff << 5);
                self.write_u32(at, word | (((distance >> 2) as u32 & 0x3fff) << 5));
            }
            FixupKind::A64Adr21 => {
                let word = self.read_u32(at) & !((0x3 << 29) | (0x7ffff << 5));
                let immlo = distance as u32 & 0x3;
                let immhi = (distance >> 2) as u32 & 0x7ffff;
                self.write_u32(at, word | (immlo << 29) | (immhi << 5));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn forward_and_backward_references_are_patched() {
        let mut buffer = CodeBuffer::new();
        let label = buffer.new_label();
        buffer.emit_u8(0xeb);
        buffer.emit_u8(0);
        assert!(buffer.add_fixup(label, 1, FixupKind::X86Rel8).is_some());
        buffer.emit_bytes(&[0x90; 3]);
        buffer.bind(label);
        buffer.emit_u8(0xe9);
        buffer.emit_u32(0);
        assert!(buffer.add_fixup(label, 6, FixupKind::X86Rel32).is_none());
        let code = buffer.finish().unwrap();
        assert_eq!(code, [0xeb, 3, 0x90, 0x90, 0x90, 0xe9, 0xfb, 0xff, 0xff, 0xff]);
    }

    #[test]
    fn unbound_label_fails() {
        let mut buffer = CodeBuffer::new();
        let label = buffer.new_label();
        buffer.emit_u32(0x1400_0000);
        buffer.add_fixup(label, 0, FixupKind::A64Branch26);
        assert_eq!(buffer.finish(), Err(AsmError::UnboundLabel));
    }

    #[test]
    fn unreferenced_unbound_label_is_fine() {
        let mut buffer = CodeBuffer::new();
        let _ = buffer.new_label();
        buffer.emit_u8(0xc3);
        assert_eq!(buffer.finish(), Ok(vec![0xc3]));
    }

    #[test]
    fn out_of_range_reference_fails() {
        let mut buffer = CodeBuffer::new();
        let label = buffer.new_label();
        buffer.emit_bytes(&[0xeb, 0]);
        buffer.add_fixup(label, 1, FixupKind::X86Rel8);
        buffer.emit_bytes(&[0x90; 128]);
        buffer.bind(label);
        assert_eq!(buffer.finish(), Err(AsmError::BranchOutOfRange));
    }

    #[test]
    fn rel8_reaches_exactly_its_range() {
        assert!(FixupKind::X86Rel8.reaches(1, 2 + 127));
        assert!(!FixupKind::X86Rel8.reaches(1, 2 + 128));
        assert!(FixupKind::X86Rel8.reaches(200, 201 - 128));
        assert!(!FixupKind::X86Rel8.reaches(200, 201 - 129));
        assert!(FixupKind::A64Branch14.reaches(0, 32764));
        assert!(!FixupKind::A64Branch14.reaches(0, 32768));
        assert!(FixupKind::A64Branch14.reaches(32768, 0));
        assert!(!FixupKind::A64Branch14.reaches(32772, 0));
    }

    #[test]
    fn alignment_pads_with_filler() {
        let mut buffer = CodeBuffer::new();
        buffer.emit_u8(0xc3);
        buffer.align_with(8, 0xcc);
        assert_eq!(
            buffer.finish().unwrap(),
            [0xc3, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc]
        );
    }
}
