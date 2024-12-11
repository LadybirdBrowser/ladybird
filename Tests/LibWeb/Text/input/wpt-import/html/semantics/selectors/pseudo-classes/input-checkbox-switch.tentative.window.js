test(t => {
  const input = document.body.appendChild(document.createElement("input"));
  t.add_cleanup(() => input.remove());
  input.type = "checkbox";
  input.switch = true;
  input.indeterminate = true;

  assert_false(input.matches(":indeterminate"));
}, "Switch control does not match :indeterminate");

test(t => {
  const input = document.body.appendChild(document.createElement("input"));
  t.add_cleanup(() => input.remove());
  input.type = "checkbox";
  input.switch = true;
  input.indeterminate = true;

  assert_false(input.matches(":indeterminate"));

  input.switch = false;
  assert_true(input.matches(":indeterminate"));
}, "Checkbox that is no longer a switch control does match :indeterminate");

test(t => {
  const input = document.body.appendChild(document.createElement("input"));
  t.add_cleanup(() => input.remove());
  input.type = "checkbox";
  input.indeterminate = true;

  assert_true(input.matches(":indeterminate"));

  input.setAttribute("switch", "blah");
  assert_false(input.matches(":indeterminate"));
}, "Checkbox that becomes a switch control does not match :indeterminate");

test(t => {
  const input = document.body.appendChild(document.createElement("input"));
  t.add_cleanup(() => input.remove());
  input.type = "checkbox";
  input.indeterminate = true;

  assert_true(document.body.matches(":has(:indeterminate)"));

  input.switch = true;
  assert_false(document.body.matches(":has(:indeterminate)"));
}, "Parent of a checkbox that becomes a switch control does not match :has(:indeterminate)");

test(t => {
  const input = document.body.appendChild(document.createElement("input"));
  t.add_cleanup(() => input.remove());
  input.type = "checkbox";
  input.switch = true
  input.checked = true;

  assert_true(document.body.matches(":has(:checked)"));

  input.switch = false;
  assert_true(document.body.matches(":has(:checked)"));

  input.checked = false;
  assert_false(document.body.matches(":has(:checked)"));
}, "Parent of a switch control that becomes a checkbox continues to match :has(:checked)");

test(t => {
  const input = document.body.appendChild(document.createElement("input"));
  t.add_cleanup(() => input.remove());
  input.type = "checkbox";
  input.switch = true;
  input.indeterminate = true;
  assert_false(input.matches(":indeterminate"));
  input.type = "text";
  input.removeAttribute("switch");
  input.type = "checkbox";
  assert_true(input.matches(":indeterminate"));
}, "A switch control that becomes a checkbox in a roundabout way");
