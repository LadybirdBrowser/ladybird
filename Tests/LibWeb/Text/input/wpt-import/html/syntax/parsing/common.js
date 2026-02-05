function mark_diffs(expected, actual) {
  var expected_lines = expected.split("\n");
  var actual_lines = actual.split("\n");

  var max_length = Math.max(expected_lines.length, actual_lines.length);

  var expected_diff = ["code", {}];
  var actual_diff = ["code", {}];

  for (var i=0; i<max_length; i++) {
    if (expected_lines[i] === actual_lines[i]) {
      expected_diff.push(expected_lines[i] + "\n");
      actual_diff.push(actual_lines[i] + "\n");
    } else {
      if (expected_lines[i]) {
        expected_diff.push(["span", {style:"color:red"}, expected_lines[i] + "\n"]);
      }
      if (actual_lines[i]) {
        actual_diff.push(["span", {style:"color:red"}, actual_lines[i] + "\n"]);
      }
    }
  }
  return [expected_diff, actual_diff];
}
