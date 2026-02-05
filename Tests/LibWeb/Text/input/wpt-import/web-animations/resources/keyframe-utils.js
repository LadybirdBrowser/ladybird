'use strict';

// =======================================
//
// Utility functions for testing keyframes
//
// =======================================


// ------------------------------
//  Helper functions
// ------------------------------

/**
 * Test equality between two lists of computed keyframes
 * @param {Array.<ComputedKeyframe>} a - actual computed keyframes
 * @param {Array.<ComputedKeyframe>} b - expected computed keyframes
 */
function assert_frame_lists_equal(a, b, message) {
  assert_equals(a.length, b.length, `number of frames: ${(message || '')}`);
  for (let i = 0; i < Math.min(a.length, b.length); i++) {
    assert_frames_equal(a[i], b[i],
                        `ComputedKeyframe #${i}: ${(message || '')}`);
  }
}

/** Helper for assert_frame_lists_equal */
function assert_frames_equal(a, b, name) {
  assert_equals(Object.keys(a).sort().toString(),
                Object.keys(b).sort().toString(),
                `properties on ${name} should match`);
  // Iterates sorted keys to ensure stable failures.
  for (const p of Object.keys(a).sort()) {
    if (typeof b[p] == 'number')
      assert_approx_equals(a[p], b[p], 1e-6, `value for '${p}' on ${name}`);
    else if (typeof b[p] == 'object') {
      for (const key in b[p]) {
        if (typeof b[p][key] == 'number') {
          assert_approx_equals(a[p][key], b[p][key], 1e-6,
                               `value for '${p}.${key}' on ${name}`);
        } else {
          assert_equals((a[p][key] || 'undefined').toString(),
                         b[p][key].toString(),
                        `value for '${p}.${key}' on ${name}`);
        }
      }
    }
    else
      assert_equals(a[p], b[p], `value for '${p}' on ${name}`);
  }
}
