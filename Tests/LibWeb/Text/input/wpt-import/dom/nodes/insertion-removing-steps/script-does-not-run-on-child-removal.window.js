// See:
//  - https://github.com/whatwg/dom/issues/808
//  - https://github.com/whatwg/dom/pull/1261
//  - https://github.com/whatwg/html/pull/10188
//  - https://source.chromium.org/chromium/chromium/src/+/604e798ec6ee30f44d57a5c4a44ce3dab3a871ed
//  - https://github.com/whatwg/dom/pull/732#pullrequestreview-328249015
//  - https://github.com/whatwg/html/pull/4354#issuecomment-476038918
test(() => {
  window.script_did_run = false;

  const script = document.createElement('script');
  // This prevents execution on insertion.
  script.type = '0';
  script.textContent = `script_did_run = true;`;
  document.body.append(script);
  assert_false(script_did_run,
      'Appending script with invalid type does not trigger execution');

  const div = document.createElement('div');
  script.append(div);
  assert_false(script_did_run,
      'Appending a child to an invalid-type script does not trigger execution');

  // This enables, but does not trigger, execution.
  script.type = '';
  assert_false(script_did_run,
      'Unsetting script type does not trigger execution');

  div.remove();
  assert_false(script_did_run,
      'Removing child from valid script that has not already run, does not ' +
      'trigger execution');
}, "Script execution is never triggered on child removals");
