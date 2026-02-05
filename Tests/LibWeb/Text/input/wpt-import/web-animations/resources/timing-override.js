// Firefox implements unconditional clamping of 20 usec; and for certain web-animation tests,
// we hit some test failures because the Time Precision is too small. We override these functions
// on a per-test basis for Firefox only.
if(navigator.userAgent.toLowerCase().indexOf('firefox') > -1){
  window.assert_times_equal = (actual, expected, description) => {
    let TIME_PRECISION = 0.02;
    assert_approx_equals(actual, expected, TIME_PRECISION * 2, description);
  };

  window.assert_time_equals_literal = (actual, expected, description) => {
    let TIME_PRECISION = 0.02;
    if (Math.abs(expected) === Infinity) {
      assert_equals(actual, expected, description);
    } else {
      assert_approx_equals(actual, expected, TIME_PRECISION, description);
    }
  }
}
