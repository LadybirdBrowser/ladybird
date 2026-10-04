/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The parts of AK/Random.cpp that the runtime needs and the ak crate does not provide: the platform CSPRNG and the
//! XorShift128+ generator that Math.random draws from.

/// AK's csprng(): fills `buffer` from the best CSPRNG of the platform.
fn csprng(buffer: &mut [u8]) {
    #[cfg(any(
        target_vendor = "apple",
        target_os = "freebsd",
        target_os = "openbsd",
        target_os = "netbsd"
    ))]
    {
        // SAFETY: The buffer is valid for writes of its whole length.
        unsafe { libc::arc4random_buf(buffer.as_mut_ptr().cast(), buffer.len()) };
    }

    #[cfg(any(target_os = "linux", target_os = "android"))]
    {
        let mut remaining = buffer;
        while !remaining.is_empty() {
            // SAFETY: The buffer is valid for writes of its whole length.
            let result = unsafe { libc::getrandom(remaining.as_mut_ptr().cast(), remaining.len(), 0) };
            // EINTR can be handled safely by just trying again. Others are fatal
            if result == -1 {
                let error = std::io::Error::last_os_error();
                assert!(error.raw_os_error() == Some(libc::EINTR), "getrandom failed: {error}");
                continue;
            }
            remaining = &mut remaining[result as usize..];
        }
    }

    #[cfg(not(any(
        target_vendor = "apple",
        target_os = "freebsd",
        target_os = "openbsd",
        target_os = "netbsd",
        target_os = "linux",
        target_os = "android"
    )))]
    compile_error!("This build target doesn't have a CSPRNG interface specified in random.rs.");
}

/// AK::get_random<u64>().
pub(crate) fn get_random_u64() -> u64 {
    let mut bytes = [0u8; 8];
    csprng(&mut bytes);
    u64::from_ne_bytes(bytes)
}

// http://vigna.di.unimi.it/ftp/papers/xorshiftplus.pdf
pub struct XorShift128PlusRNG {
    low: u64,
    high: u64,
}

impl XorShift128PlusRNG {
    #[allow(
        clippy::new_without_default,
        reason = "every generator is seeded from the CSPRNG, as AK's is"
    )]
    pub fn new() -> Self {
        // Splitmix64 is used as xorshift is sensitive to being seeded with all 0s
        let mut seed = get_random_u64();
        let low = Self::splitmix64(&mut seed);
        seed = get_random_u64();
        let high = Self::splitmix64(&mut seed);
        Self { low, high }
    }

    pub fn get(&mut self) -> f64 {
        let value = self.advance() & ((1u64 << 53) - 1);
        value as f64 * (1.0 / (1u64 << 53) as f64)
    }

    fn splitmix64(state: &mut u64) -> u64 {
        *state = state.wrapping_add(0x9e37_79b9_7f4a_7c15);
        let mut z = *state;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        z ^ (z >> 31)
    }

    // Apparently this set of constants is better: https://stackoverflow.com/a/34432126
    fn advance(&mut self) -> u64 {
        let mut s1 = self.low;
        let s0 = self.high;
        let result = s0.wrapping_add(s1);
        self.low = s0;
        s1 ^= s1 << 23;
        s1 ^= s1 >> 18;
        s1 ^= s0 ^ (s0 >> 5);
        self.high = s1;
        result.wrapping_add(s1)
    }
}
