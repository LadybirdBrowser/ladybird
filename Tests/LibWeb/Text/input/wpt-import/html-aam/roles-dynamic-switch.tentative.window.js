// META: script=/resources/testdriver.js
// META: script=/resources/testdriver-vendor.js
// META: script=/resources/testdriver-actions.js

promise_test(async () => {
  const control = document.createElement("input");
  control.type = "checkbox";
  control.switch = true;
  const role = await test_driver.get_computed_role(control);
  assert_equals(role, "");
}, `Disconnected <input type=checkbox switch>`);

promise_test(async t => {
  const control = document.createElement("input");
  t.add_cleanup(() => control.remove());
  control.type = "checkbox";
  control.switch = true;
  document.body.append(control);
  const role = await test_driver.get_computed_role(control);
  assert_equals(role, "switch");
}, `Connected <input type=checkbox switch>`);

promise_test(async t => {
  const control = document.createElement("input");
  t.add_cleanup(() => control.remove());
  control.type = "checkbox";
  document.body.append(control);
  let role = await test_driver.get_computed_role(control);
  assert_equals(role, "checkbox");
  control.switch = true;
  role = await test_driver.get_computed_role(control);
  assert_equals(role, "switch");
}, `Connected <input type=checkbox switch>: adding switch attribute`);

promise_test(async t => {
  const control = document.createElement("input");
  t.add_cleanup(() => control.remove());
  control.type = "checkbox";
  control.switch = true;
  document.body.append(control);
  let role = await test_driver.get_computed_role(control);
  assert_equals(role, "switch");
  control.switch = false;
  role = await test_driver.get_computed_role(control);
  assert_equals(role, "checkbox");
}, `Connected <input type=checkbox switch>: removing switch attribute`);

promise_test(async t => {
  const control = document.createElement("input");
  t.add_cleanup(() => control.remove());
  control.type = "checkbox";
  document.body.append(control);
  control.switch = true;
  let role = await test_driver.get_computed_role(control);
  assert_equals(role, "switch");
  control.removeAttribute("type");
  role = await test_driver.get_computed_role(control);
  assert_equals(role, "textbox");
}, `Connected <input type=checkbox switch>: removing type attribute`);

promise_test(async t => {
  const control = document.createElement("input");
  t.add_cleanup(() => control.remove());
  control.switch = true;
  document.body.append(control);
  let role = await test_driver.get_computed_role(control);
  assert_equals(role, "textbox");
  control.type = "checkbox";
  role = await test_driver.get_computed_role(control);
  assert_equals(role, "switch");
}, `Connected <input type=checkbox switch>: adding type attribute`);
