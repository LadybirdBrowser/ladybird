test(t => {
  const input = document.createElement("input");
  input.switch = true;

  assert_true(input.hasAttribute("switch"));
  assert_equals(input.getAttribute("switch"), "");
  assert_equals(input.type, "text");
}, "switch IDL attribute, setter");

test(t => {
  const container = document.createElement("div");
  container.innerHTML = "<input type=checkbox switch>";
  const input = container.firstChild;

  assert_true(input.hasAttribute("switch"));
  assert_equals(input.getAttribute("switch"), "");
  assert_equals(input.type, "checkbox");
  assert_true(input.switch);
}, "switch IDL attribute, getter");
