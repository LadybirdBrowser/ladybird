// META: title=MutationObserver: callback order
"use strict";

// Mutation observers are notified in the order they were created, not in the order in which they
// got records.

// Mutation observers are notified in a microtask, so they have been notified once a new task runs.
function nextTask(t) {
  return new Promise(resolve => t.step_timeout(resolve, 0));
}

promise_test(async t => {
  const firstTarget = document.createElement("div");
  const secondTarget = document.createElement("div");
  const order = [];
  const first = new MutationObserver(() => order.push("first"));
  const second = new MutationObserver(() => order.push("second"));
  first.observe(firstTarget, { attributes: true });
  second.observe(secondTarget, { attributes: true });

  secondTarget.setAttribute("data-test", "");
  firstTarget.setAttribute("data-test", "");
  await nextTask(t);

  assert_array_equals(order, ["first", "second"]);
}, "Mutation observers are notified in the order they were created");

promise_test(async t => {
  const parent = document.createElement("div");
  const child = parent.appendChild(document.createElement("div"));
  const order = [];
  const first = new MutationObserver(() => order.push("first"));
  const second = new MutationObserver(() => order.push("second"));
  first.observe(parent, { childList: true });
  second.observe(parent, { attributes: true, subtree: true });

  // This queues a record for first, while second keeps observing child.
  child.remove();
  child.setAttribute("data-test", "");
  await nextTask(t);

  assert_array_equals(order, ["first", "second"]);
}, "Mutation observers are notified in the order they were created, also when one observes a removed subtree");

promise_test(async t => {
  const target = document.createElement("div");
  const order = [];
  const observer = new MutationObserver(() => order.push("observer"));
  observer.observe(target, { attributes: true });

  // No observer gets a record for this mutation, so it does not queue the mutation observer
  // microtask.
  document.createElement("div").append("text");
  Promise.resolve().then(() => order.push("promise"));
  // This queues a record and therefore the mutation observer microtask, after the promise job.
  target.setAttribute("data-test", "");
  await nextTask(t);

  assert_array_equals(order, ["promise", "observer"]);
}, "The mutation observer microtask is only queued once a record is queued for an observer");
