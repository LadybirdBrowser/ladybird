test(() => {
  const detached = document.createElement("div");
  const walker = document.createTreeWalker(document, 0, null);
  walker.currentNode = detached;
  assert_equals(walker.nextNode(), null);
}, "nextNode() returns null when currentNode is a detached element with no descendants and whatToShow does not match it");

test(() => {
  const detached = document.createElement("div");
  detached.appendChild(document.createElement("span"));
  const walker = document.createTreeWalker(document, NodeFilter.SHOW_ELEMENT, null);
  walker.currentNode = detached;
  assert_equals(walker.nextNode(), detached.firstChild);
  assert_equals(walker.nextNode(), null);
}, "nextNode() returns null after exhausting a detached subtree");

test(() => {
  const template = document.createElement("template");
  template.innerHTML = "text<div></div>text";
  const walker = document.createTreeWalker(
    document,
    NodeFilter.SHOW_ELEMENT | NodeFilter.SHOW_COMMENT,
    null);
  walker.currentNode = template.content;
  assert_equals(walker.nextNode(), template.content.children[0]);
  assert_equals(walker.nextNode(), null);
}, "nextNode() terminates when iterating over template.content not under the walker's root");
