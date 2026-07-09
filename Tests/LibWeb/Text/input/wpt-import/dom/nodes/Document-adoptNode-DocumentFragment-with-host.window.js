// Tests for adopt() + DocumentFragment-with-host. See whatwg/dom#813, #814.
//
// A DocumentFragment can have a non-null host in two cases:
//   - a shadow root (host is the shadow host element), and
//   - a template element's content fragment (host is the <template> element).
//
// adoptNode() throws for a shadow root (HierarchyRequestError), but otherwise a
// DocumentFragment with a host -- i.e. a template's content -- is adopted like
// any other node and returned.

"use strict";

function newInertDoc() {
  return document.implementation.createHTMLDocument("");
}

// Baseline: plain DocumentFragment (host is null).
test(() => {
  const df = document.createDocumentFragment();
  const child = df.appendChild(document.createElement("span"));
  const target = newInertDoc();

  const result = target.adoptNode(df);

  assert_equals(result, df, "returns the fragment");
  assert_equals(df.ownerDocument, target, "fragment adopted");
  assert_equals(df.childNodes.length, 1, "children preserved");
  assert_equals(child.ownerDocument, target, "child adopted");
}, "adoptNode() plain DocumentFragment (no host): adopts and returns it");

// adoptNode(template.content): return value (issue #813) and side effects into
// an inert target document.
test(() => {
  const template = document.createElement("template");
  const child = template.content.appendChild(document.createElement("span"));
  const target = newInertDoc();
  assert_not_equals(template.content.ownerDocument, target,
    "precondition: content starts in a different document");

  const result = target.adoptNode(template.content);

  assert_equals(result, template.content,
    "returns the fragment (issue #813: adoptNode must return a Node)");
  assert_equals(template.content.ownerDocument, target, "fragment adopted");
  assert_equals(template.content.childNodes.length, 1, "children preserved");
  assert_equals(child.ownerDocument, target, "child adopted");
  assert_equals(template.ownerDocument, document,
    "the <template> element itself is not touched");
}, "adoptNode() template.content: adopts fragment + children, returns it");

// adoptNode(template.content) into a document with a browsing context: the
// fragment lands on that document directly (no template-contents-owner
// redirection, which only applies when the <template> element is adopted).
test(() => {
  const source = newInertDoc();
  const template = source.createElement("template");
  const child = template.content.appendChild(source.createElement("span"));

  const result = document.adoptNode(template.content);

  assert_equals(result, template.content, "returns the fragment");
  assert_equals(template.content.ownerDocument, document,
    "fragment adopted directly into the browsing-context document");
  assert_equals(child.ownerDocument, document, "child adopted");
}, "adoptNode() template.content into browsing-context document");

// adoptNode(shadowRoot): a DF-with-host, but shadow roots throw.
test(() => {
  const host = document.createElement("div");
  const root = host.attachShadow({ mode: "closed" });
  const target = newInertDoc();
  assert_throws_dom("HierarchyRequestError", () => target.adoptNode(root));
}, "adoptNode() shadow root: throws HierarchyRequestError");

// Adopting the <template> element (not its content) runs HTML's template
// adopting steps: content follows into the new node document's template
// contents owner document.
test(() => {
  const source = newInertDoc();
  const template = source.createElement("template");
  const child = template.content.appendChild(source.createElement("span"));
  const originalContentDoc = template.content.ownerDocument;

  const target = newInertDoc();
  const result = target.adoptNode(template);

  assert_equals(result, template, "returns the template element");
  assert_equals(template.ownerDocument, target,
    "template element adopted into the target");
  assert_not_equals(template.content.ownerDocument, originalContentDoc,
    "content moved to a new template-contents owner document");
  assert_equals(child.ownerDocument, template.content.ownerDocument,
    "descendants track the content's owner document");
}, "adoptNode() <template> element: content follows via template adopting steps");

// Nested template: a template whose content contains another template.
test(() => {
  const source = newInertDoc();
  const outer = source.createElement("template");
  const inner = source.createElement("template");
  const innerChild = inner.content.appendChild(source.createElement("span"));
  outer.content.appendChild(inner);

  const target = newInertDoc();
  target.adoptNode(outer);

  assert_equals(outer.ownerDocument, target, "outer template adopted");
  assert_equals(inner.ownerDocument, outer.content.ownerDocument,
    "inner template lives in outer's content owner document");
  assert_equals(inner.content.ownerDocument, outer.content.ownerDocument,
    "inner content owner document tracks outer content owner document");
  assert_equals(innerChild.ownerDocument, outer.content.ownerDocument,
    "deep descendant owner document tracks the content owner document");
}, "adoptNode() nested <template> elements: all owner documents update");

// appendChild(template.content): inserting a DF-with-host extracts its children;
// the fragment itself is not inserted, so its ownerDocument stays put.
test(() => {
  const source = newInertDoc();
  const template = source.createElement("template");
  const child = template.content.appendChild(source.createElement("span"));
  const originalContentDoc = template.content.ownerDocument;

  document.body.appendChild(template.content);

  assert_equals(template.content.childNodes.length, 0, "children moved out");
  assert_equals(child.ownerDocument, document, "moved child adopted");
  assert_equals(template.content.ownerDocument, originalContentDoc,
    "fragment ownerDocument unchanged (fragment itself was not inserted)");
  child.remove();
}, "appendChild() template.content: moves children, fragment ownerDocument stays");
