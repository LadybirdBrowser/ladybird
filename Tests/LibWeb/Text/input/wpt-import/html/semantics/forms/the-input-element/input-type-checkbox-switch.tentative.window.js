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

test(t => {
  const input = document.createElement("input");
  input.type = "checkbox";
  input.switch = true;

  const clone = input.cloneNode();
  assert_equals(clone.getAttribute("switch"), "");
  assert_equals(clone.type, "checkbox");
  assert_true(clone.switch);

  t.add_cleanup(() => clone.remove());
  document.body.appendChild(clone);
  assert_equals(clone.getAttribute("switch"), "");
  assert_equals(clone.type, "checkbox");
  assert_true(clone.switch);
}, "Cloning a switch control");
