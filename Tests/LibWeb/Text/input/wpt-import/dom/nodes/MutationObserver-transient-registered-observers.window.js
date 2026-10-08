// META: title=MutationObserver: transient registered observers
"use strict";

// When a node is removed from a subtree that is observed with subtree set to true, the node and its
// descendants remain observed until mutation observers are notified.

function createTree() {
  const parent = document.createElement("div");
  parent.id = "parent";
  const child = parent.appendChild(document.createElement("div"));
  child.id = "child";
  const grandchild = child.appendChild(document.createElement("div"));
  grandchild.id = "grandchild";
  return { parent, child, grandchild };
}

function createElement(id) {
  const element = document.createElement("span");
  element.id = id;
  return element;
}

function summarize(records) {
  return records.map(({ type, target, addedNodes, removedNodes, attributeName }) => {
    let summary = `${type} ${target.id}`;
    for (const node of addedNodes) {
      summary += ` +${node.id}`;
    }
    for (const node of removedNodes) {
      summary += ` -${node.id}`;
    }
    if (attributeName !== null) {
      summary += ` ${attributeName}`;
    }
    return summary;
  });
}

// Mutation observers are notified in a microtask, so they have been notified once a new task runs.
function nextTask(t) {
  return new Promise(resolve => t.step_timeout(resolve, 0));
}

test(() => {
  const { parent, child } = createTree();
  const observer = new MutationObserver(() => {});
  observer.observe(parent, { childList: true, subtree: true });

  child.remove();
  child.append(createElement("added"));

  assert_array_equals(summarize(observer.takeRecords()), [
    "childList parent -child",
    "childList child +added"
  ]);
}, "Mutations in a removed subtree are observed until mutation observers are notified");

test(() => {
  const { parent, child } = createTree();
  const observer = new MutationObserver(() => {});
  observer.observe(parent, { attributes: true, subtree: true });

  child.remove();
  child.setAttribute("data-test", "");

  assert_array_equals(summarize(observer.takeRecords()), ["attributes child data-test"]);
}, "Attribute mutations in a removed subtree are observed until mutation observers are notified");

test(() => {
  const { parent, child } = createTree();
  const observer = new MutationObserver(() => {});
  observer.observe(parent, { childList: true, subtree: true });

  parent.replaceChildren();
  child.append(createElement("added"));

  assert_array_equals(summarize(observer.takeRecords()), [
    "childList parent -child",
    "childList child +added"
  ]);
}, "Mutations in a subtree removed by replaceChildren() are observed until mutation observers are notified");

test(() => {
  const { parent, child } = createTree();
  const observer = new MutationObserver(() => {});
  observer.observe(parent, { childList: true, subtree: true });

  child.remove();
  assert_array_equals(summarize(observer.takeRecords()), ["childList parent -child"]);

  child.append(createElement("added"));
  assert_array_equals(summarize(observer.takeRecords()), ["childList child +added"]);
}, "takeRecords() does not stop observing a removed subtree");

promise_test(async t => {
  const { parent, child } = createTree();
  const delivered = [];
  const observer = new MutationObserver(records => delivered.push(...summarize(records)));
  observer.observe(parent, { childList: true, subtree: true });

  // This microtask is queued before the mutation observer microtask and therefore runs first.
  Promise.resolve().then(() => child.append(createElement("before")));
  child.remove();
  await nextTask(t);
  assert_array_equals(delivered, ["childList parent -child", "childList child +before"]);

  child.append(createElement("after"));
  assert_array_equals(summarize(observer.takeRecords()), []);
}, "A removed subtree is no longer observed after mutation observers are notified");

promise_test(async t => {
  const { parent, child } = createTree();
  const observer = new MutationObserver(t.unreached_func("callback invoked"));
  observer.observe(parent, { attributes: true, subtree: true });

  // This does not queue a record as observer does not observe childList mutations.
  child.remove();
  await nextTask(t);

  child.setAttribute("data-test", "");
  assert_array_equals(summarize(observer.takeRecords()), []);
}, "A removed subtree is no longer observed after mutation observers are notified, even if no record was queued");

promise_test(async t => {
  const { parent, child } = createTree();
  const observer = new MutationObserver(t.unreached_func("callback invoked"));
  observer.observe(parent, { childList: true, subtree: true });

  child.remove();
  assert_array_equals(summarize(observer.takeRecords()), ["childList parent -child"]);
  await nextTask(t);

  child.append(createElement("added"));
  assert_array_equals(summarize(observer.takeRecords()), []);
}, "A removed subtree is no longer observed after mutation observers are notified, even if the callback was not invoked");

promise_test(async t => {
  const { parent, child, grandchild } = createTree();
  const observer = new MutationObserver(() => {});
  observer.observe(parent, { childList: true, subtree: true });

  child.remove();
  // grandchild is removed from a subtree that is itself only observed because it was removed.
  grandchild.remove();
  grandchild.append(createElement("before"));
  assert_array_equals(summarize(observer.takeRecords()), [
    "childList parent -child",
    "childList child -grandchild",
    "childList grandchild +before"
  ]);
  await nextTask(t);

  grandchild.append(createElement("after"));
  assert_array_equals(summarize(observer.takeRecords()), []);
}, "A subtree removed from a removed subtree is no longer observed after mutation observers are notified");

promise_test(async t => {
  const { parent, child } = createTree();
  const order = [];
  const observer = new MutationObserver(() => order.push("observer"));
  observer.observe(parent, { attributes: true, subtree: true });

  // This queues no record, as observer does not observe childList mutations.
  child.remove();
  Promise.resolve().then(() => order.push("promise"));
  child.setAttribute("data-test", "");
  await nextTask(t);

  assert_array_equals(order, ["observer", "promise"]);
}, "Removing a node from an observed subtree queues the mutation observer microtask");

test(() => {
  const { parent, child } = createTree();
  const observer = new MutationObserver(() => {});
  observer.observe(parent, { childList: true, subtree: true });

  child.remove();
  observer.disconnect();
  child.append(createElement("added"));

  assert_array_equals(summarize(observer.takeRecords()), []);
}, "disconnect() stops observing a removed subtree");

test(() => {
  const { parent, child, grandchild } = createTree();
  const observer = new MutationObserver(() => {});
  observer.observe(parent, { childList: true, subtree: true });

  child.remove();
  grandchild.remove();
  observer.disconnect();
  child.append(createElement("added-to-child"));
  grandchild.append(createElement("added-to-grandchild"));

  assert_array_equals(summarize(observer.takeRecords()), []);
}, "disconnect() stops observing a subtree removed from a removed subtree");

test(() => {
  const { parent, child } = createTree();
  const observer = new MutationObserver(() => {});
  const otherObserver = new MutationObserver(() => {});
  observer.observe(parent, { childList: true, subtree: true });
  otherObserver.observe(parent, { childList: true, subtree: true });

  child.remove();
  observer.disconnect();
  child.append(createElement("added"));

  assert_array_equals(summarize(observer.takeRecords()), []);
  assert_array_equals(summarize(otherObserver.takeRecords()), [
    "childList parent -child",
    "childList child +added"
  ]);
}, "disconnect() does not affect another observer observing a removed subtree");

test(() => {
  const { parent, child, grandchild } = createTree();
  const observer = new MutationObserver(() => {});
  observer.observe(parent, { childList: true, subtree: true });
  observer.observe(child, { attributes: true, subtree: true });

  // grandchild remains observed through both parent and child.
  grandchild.remove();
  // This stops observing grandchild through child, but not through parent.
  observer.observe(child, { attributes: true, subtree: true });
  grandchild.append(createElement("added"));
  grandchild.setAttribute("data-test", "");

  assert_array_equals(summarize(observer.takeRecords()), [
    "childList child -grandchild",
    "childList grandchild +added"
  ]);
}, "observe() stops observing a removed subtree through the target, but not through other targets");

test(() => {
  const { parent, child, grandchild } = createTree();
  const observer = new MutationObserver(() => {});
  observer.observe(parent, { childList: true, subtree: true });

  child.remove();
  // grandchild is removed from a subtree that is itself only observed because it was removed.
  grandchild.remove();
  observer.observe(parent, { childList: true, subtree: true });
  child.append(createElement("added-to-child"));
  grandchild.append(createElement("added-to-grandchild"));

  assert_array_equals(summarize(observer.takeRecords()), [
    "childList parent -child",
    "childList child -grandchild"
  ]);
}, "observe() stops observing subtrees removed from a removed subtree");

promise_test(async t => {
  const { parent, child } = createTree();
  const observer = new MutationObserver(() => {});
  observer.observe(parent, { childList: true, subtree: true });

  child.remove();
  observer.observe(child, { attributes: true });
  await nextTask(t);

  child.append(createElement("added"));
  child.setAttribute("data-test", "");
  assert_array_equals(summarize(observer.takeRecords()), ["attributes child data-test"]);
}, "observe() on a node in a removed subtree keeps observing it after mutation observers are notified");
