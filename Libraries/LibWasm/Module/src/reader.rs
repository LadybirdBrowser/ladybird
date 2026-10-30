/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A cursor over the bytes of a binary module.

use crate::error::{Error, Result};

#[derive(Clone)]
pub struct Reader<'a> {
    bytes: &'a [u8],
    position: usize,
    end: usize,
}

impl<'a> Reader<'a> {
    pub fn new(bytes: &'a [u8]) -> Self {
        Self {
            bytes,
            position: 0,
            end: bytes.len(),
        }
    }

    /// A reader over `bytes[start..end]` that reports positions relative to the start of `bytes`.
    pub fn with_range(bytes: &'a [u8], start: usize, end: usize) -> Self {
        Self {
            bytes,
            position: start,
            end,
        }
    }

    pub fn bytes(&self) -> &'a [u8] {
        self.bytes
    }

    pub fn position(&self) -> usize {
        self.position
    }

    pub fn end(&self) -> usize {
        self.end
    }

    pub fn is_at_end(&self) -> bool {
        self.position >= self.end
    }

    pub fn remaining(&self) -> usize {
        self.end - self.position
    }

    /// A reader over the next `length` bytes, which this reader skips.
    pub fn sub_reader(&mut self, length: usize) -> Result<Reader<'a>> {
        if length > self.remaining() {
            return Err(Error::malformed("unexpected end of section or function"));
        }
        let reader = Reader::with_range(self.bytes, self.position, self.position + length);
        self.position += length;
        Ok(reader)
    }

    pub fn read_u8(&mut self) -> Result<u8> {
        if self.position >= self.end {
            return Err(Error::malformed("unexpected end"));
        }
        let byte = self.bytes[self.position];
        self.position += 1;
        Ok(byte)
    }

    pub fn peek_u8(&self) -> Result<u8> {
        if self.position >= self.end {
            return Err(Error::malformed("unexpected end"));
        }
        Ok(self.bytes[self.position])
    }

    pub fn read_bytes(&mut self, length: usize) -> Result<&'a [u8]> {
        if length > self.remaining() {
            return Err(Error::malformed("unexpected end"));
        }
        let bytes = &self.bytes[self.position..self.position + length];
        self.position += length;
        Ok(bytes)
    }

    pub fn read_fixed_u32(&mut self) -> Result<u32> {
        let bytes = self.read_bytes(4)?;
        Ok(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }

    pub fn read_fixed_u64(&mut self) -> Result<u64> {
        let bytes = self.read_bytes(8)?;
        let mut array = [0; 8];
        array.copy_from_slice(bytes);
        Ok(u64::from_le_bytes(array))
    }

    fn read_unsigned_leb(&mut self, bits: u32) -> Result<u64> {
        let mut result = 0u64;
        let mut shift = 0;
        loop {
            let byte = self.read_u8().map_err(|_| Error::malformed("unexpected end"))?;
            let payload = u64::from(byte & 0x7f);
            let remaining_bits = bits - shift;
            if remaining_bits < 7 {
                // The final byte may not set bits beyond the integer's width.
                if byte & 0x80 != 0 || payload >> remaining_bits != 0 {
                    return Err(Error::malformed(if byte & 0x80 != 0 {
                        "integer representation too long"
                    } else {
                        "integer too large"
                    }));
                }
            }
            result |= payload << shift;
            if byte & 0x80 == 0 {
                return Ok(result);
            }
            shift += 7;
        }
    }

    fn read_signed_leb(&mut self, bits: u32) -> Result<i64> {
        let mut result = 0i64;
        let mut shift = 0;
        loop {
            let byte = self.read_u8().map_err(|_| Error::malformed("unexpected end"))?;
            let payload = i64::from(byte & 0x7f);
            let remaining_bits = bits - shift;
            if remaining_bits < 7 {
                // The unused bits of the final byte must all equal its sign bit.
                let sign_and_unused = (byte & 0x7f) >> (remaining_bits - 1);
                let expected = if byte & 0x40 != 0 {
                    0x7f >> (remaining_bits - 1)
                } else {
                    0
                };
                if byte & 0x80 != 0 {
                    return Err(Error::malformed("integer representation too long"));
                }
                if sign_and_unused != expected {
                    return Err(Error::malformed("integer too large"));
                }
            }
            result |= payload << shift;
            shift += 7;
            if byte & 0x80 == 0 {
                if shift < 64 && byte & 0x40 != 0 {
                    result |= -1i64 << shift;
                }
                return Ok(result);
            }
        }
    }

    pub fn read_u32(&mut self) -> Result<u32> {
        Ok(self.read_unsigned_leb(32)? as u32)
    }

    pub fn read_u64(&mut self) -> Result<u64> {
        self.read_unsigned_leb(64)
    }

    pub fn read_i32(&mut self) -> Result<i32> {
        Ok(self.read_signed_leb(32)? as i32)
    }

    pub fn read_i64(&mut self) -> Result<i64> {
        self.read_signed_leb(64)
    }

    pub fn read_s33(&mut self) -> Result<i64> {
        self.read_signed_leb(33)
    }

    pub fn read_name(&mut self) -> Result<&'a str> {
        let length = self.read_u32()? as usize;
        let bytes = self.read_bytes(length)?;
        std::str::from_utf8(bytes).map_err(|_| Error::malformed("malformed UTF-8 encoding"))
    }

    /// The length of a vector, rejected early if it cannot fit in the remaining bytes, given that every element
    /// takes at least one byte.
    pub fn read_vector_length(&mut self) -> Result<u32> {
        let length = self.read_u32()?;
        if length as usize > self.remaining() {
            return Err(Error::malformed("unexpected end of section or function"));
        }
        Ok(length)
    }

    /// A vector of elements, each read by `read_element`. Its storage is reserved up front, and a length whose storage
    /// cannot be allocated is an error rather than an abort.
    pub fn read_vector<T>(&mut self, mut read_element: impl FnMut(&mut Self) -> Result<T>) -> Result<Vec<T>> {
        let length = self.read_vector_length()?;
        let mut elements = Vec::new();
        elements
            .try_reserve_exact(length as usize)
            .map_err(|_| Error::malformed("vector too large"))?;
        for _ in 0..length {
            elements.push(read_element(self)?);
        }
        Ok(elements)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_leb128() {
        assert_eq!(Reader::new(&[0x7f]).read_u32().unwrap(), 127);
        assert_eq!(Reader::new(&[0x80, 0x01]).read_u32().unwrap(), 128);
        assert_eq!(
            Reader::new(&[0xff, 0xff, 0xff, 0xff, 0x0f]).read_u32().unwrap(),
            u32::MAX
        );
        assert!(Reader::new(&[0xff, 0xff, 0xff, 0xff, 0x1f]).read_u32().is_err());
        assert!(Reader::new(&[0x80, 0x80, 0x80, 0x80, 0x80, 0x00]).read_u32().is_err());
        assert_eq!(Reader::new(&[0x7f]).read_i32().unwrap(), -1);
        assert_eq!(Reader::new(&[0x80, 0x7f]).read_i32().unwrap(), -128);
        assert_eq!(
            Reader::new(&[0xff, 0xff, 0xff, 0xff, 0x07]).read_i32().unwrap(),
            i32::MAX
        );
        assert_eq!(
            Reader::new(&[0x80, 0x80, 0x80, 0x80, 0x78]).read_i32().unwrap(),
            i32::MIN
        );
        assert!(Reader::new(&[0xff, 0xff, 0xff, 0xff, 0x0f]).read_i32().is_err());
        assert!(Reader::new(&[0x80, 0x80, 0x80, 0x80, 0x70]).read_i32().is_err());
        assert_eq!(
            Reader::new(&[0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x7f])
                .read_i64()
                .unwrap(),
            i64::MIN
        );
        assert_eq!(Reader::new(&[0x40]).read_s33().unwrap(), -64);
    }
}
