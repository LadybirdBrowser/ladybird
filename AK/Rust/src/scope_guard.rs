/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[must_use = "if unused, the scope guard is dropped immediately"]
pub struct ScopeGuard<F: FnMut()> {
    callback: F,
}

impl<F: FnMut()> ScopeGuard<F> {
    pub fn new(callback: F) -> Self {
        Self { callback }
    }
}

impl<F: FnMut()> Drop for ScopeGuard<F> {
    fn drop(&mut self) {
        (self.callback)();
    }
}

#[cfg(test)]
mod tests {
    use super::ScopeGuard;

    #[test]
    fn runs_callback_when_dropped() {
        let mut call_count = 0;
        {
            let _guard = ScopeGuard::new(|| call_count += 1);
        }
        assert_eq!(call_count, 1);
    }
}
