// META: title=moveBefore() and MutationObserver: a moved subtree remains observed until notification
"use strict";

promise_test(async t => {
  const root = document.createElement("div");
  const oldParent = root.appendChild(document.createElement("div"));
  const newParent = root.appendChild(document.createElement("div"));
  const target = oldParent.appendChild(document.createElement("div"));
  const observer = new MutationObserver(() => {});
  observer.observe(oldParent, { childList: true, subtree: true });

  newParent.moveBefore(target, null);
  const before = target.appendChild(document.createElement("span"));
  const records = observer.takeRecords();
  assert_equals(records.length, 2, "number of records");
  assert_equals(records[0].target, oldParent, "removal record target");
  assert_array_equals([...records[0].removedNodes], [target], "removal record removedNodes");
  assert_equals(records[1].target, target, "second record target");
  assert_array_equals([...records[1].addedNodes], [before], "second record addedNodes");

  // Mutation observers are notified in a microtask, so they have been notified once a new task runs.
  await new Promise(resolve => t.step_timeout(resolve, 0));

  target.append(document.createElement("span"));
  assert_array_equals(observer.takeRecords(), [], "records after mutation observers are notified");
}, "A subtree moved out of an observed subtree by moveBefore() is observed until mutation observers are notified");
