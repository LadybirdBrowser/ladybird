test(() => {
  const input = document.createElement("input");
  input.type = "color";
  input.setAttribute("alpha", "blah");
  assert_equals(input.getAttribute("alpha"), "blah");
  assert_true(input.alpha);
  input.alpha = false;
  assert_false(input.hasAttribute("alpha"));
  input.alpha = "blah";
  assert_true(input.alpha);
  assert_equals(input.getAttribute("alpha"), "");
}, "<input type=color>: alpha attribute");

test(() => {
  const input = document.createElement("input");
  input.type = "color";
  input.setAttribute("colorspace", "blah");
  assert_equals(input.getAttribute("colorspace"), "blah");
  assert_equals(input.colorSpace, "limited-srgb");
  input.colorSpace = null;
  assert_equals(input.getAttribute("colorspace"), "null");
  assert_equals(input.colorSpace, "limited-srgb");
  input.colorSpace = "DISPLAY-P3";
  assert_equals(input.getAttribute("colorspace"), "DISPLAY-P3");
  assert_equals(input.colorSpace, "display-p3");
  input.colorSpace = "DıSPLAY-P3";
  assert_equals(input.getAttribute("colorspace"), "DıSPLAY-P3");
  assert_equals(input.colorSpace, "limited-srgb");
}, "<input type=color>: colorspace attribute");
