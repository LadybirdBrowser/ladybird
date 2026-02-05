// META: script=./resources/targetted-form.js

test(t => {
  const form = populateForm('<input required><input type=submit>');
  t.add_cleanup(() => {
    form.previousElementSibling.remove();
    form.remove();
  });
  const submitter = form.querySelector('input[type=submit]');
  let invalid = form.querySelector('[required]');
  let targets = [];
  const listener = e => targets.push(e.target.localName);
  form.addEventListener("invalid", t.step_func(listener));
  form.oninvalid = t.step_func(listener);
  invalid.addEventListener("invalid", t.step_func(listener));
  invalid.oninvalid = t.step_func(listener);
  submitter.click();
  assert_array_equals(targets, ["input", "input"]);
}, "invalid event is only supported for form controls");
