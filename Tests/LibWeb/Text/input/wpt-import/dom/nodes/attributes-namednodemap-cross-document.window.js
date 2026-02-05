"use strict";

test(() => {
  const element = document.createElement("div");
  element.setAttribute("x", "first");
  const attribute = element.attributes[0];
  assert_equals(attribute.ownerDocument, document);

  const otherDocument = new Document();
  const otherElement = otherDocument.createElement("other");
  assert_throws_dom("InUseAttributeError", () => otherElement.attributes.setNamedItem(attribute));

  element.removeAttribute("x");
  otherElement.attributes.setNamedItem(attribute);
  assert_equals(attribute.ownerDocument, otherDocument);
}, "Moving an attribute between documents");

test(() => {
  const element = document.createElement("div");
  element.setAttribute("x", "first");
  const attribute = element.attributes[0];
  element.removeAttribute("x");

  const otherDocument = new Document();
  const otherElement = otherDocument.createElement("other");
  otherElement.setAttribute("x", "second");

  otherElement.attributes.setNamedItem(attribute);
  assert_equals(attribute.ownerDocument, otherDocument);
  assert_equals(otherElement.getAttribute("x"), "first");
}, "Replacing an attribute across documents");
