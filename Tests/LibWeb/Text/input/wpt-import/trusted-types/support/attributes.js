const policyWithoutModification =
  window.trustedTypes.createPolicy('policyWithoutModification', {
    createHTML: s => s,
    createScript: s => s,
    createScriptURL: s => s,
  });

function createTrustedOutput(type, input) {
  return type ?
    policyWithoutModification[type.replace("Trusted", "create")](input) : input;
}

// This is an array of test cases for testing callers to the
// "Get Trusted Type data for attribute" algorithm e.g. via
// Element.setAttributeNS() or TrustedTypePolicyFactory.getAttributeType().
// https://w3c.github.io/trusted-types/dist/spec/#abstract-opdef-get-trusted-type-data-for-attribute
//
// The test cases are described by objects with the following members:
// - element: a function creating a DOM Element in current document.
// - attrNS: the namespace of the attribute.
// - attrName: the local name of the attribute.
// - type: the required trusted type of the element's attribute as one of the
//   string "TrustedHTML", "TrustedScript", "TrustedScriptURL" ; or null if the
//   this element's attribute does not correspond to any trusted type sink.
// - sink (optional): If element's attribute is a trusted type sink, this is
//   a string with the corresponding sink name.
const trustedTypeDataForAttribute = [
  // Valid trusted type sinks for attributes as defined by
  // https://w3c.github.io/trusted-types/dist/spec/#abstract-opdef-get-trusted-type-data-for-attribute
  // For a more complete coverage of event handler content attributes, see
  // set-event-handlers-content-attributes.tentative.html and
  // TrustedTypePolicyFactory-getAttributeType-event-handler-content-attributes.tentative.html
  {
    element: _ => document.createElement("div"),
    attrNS: null,
    attrName: "onclick",
    type: "TrustedScript",
    sink: "Element onclick"
  },
  {
    element: _ => document.createElementNS(NSURI_SVG, "g"),
    attrNS: null,
    attrName: "ondblclick",
    type: "TrustedScript",
    sink: "Element ondblclick"
  },
  {
    element: _ => document.createElementNS(NSURI_MATHML, "mrow"),
    attrNS: null,
    attrName: "onmousedown",
    type: "TrustedScript",
    sink: "Element onmousedown"
  },
  {
    element: _ => document.createElement("iframe"),
    attrNS: null,
    attrName: "srcdoc",
    type: "TrustedHTML",
    sink: "HTMLIFrameElement srcdoc",
  },
  {
    element: _ => document.createElement("script"),
    attrNS: null,
    attrName: "src",
    type: "TrustedScriptURL",
    sink: "HTMLScriptElement src",
  },
  {
    element: _ => document.createElementNS(NSURI_SVG, "script"),
    attrNS: null,
    attrName: "href",
    type: "TrustedScriptURL",
    sink: "SVGScriptElement href",
  },
  {
    element: _ => document.createElementNS(NSURI_SVG, "script"),
    attrNS: NSURI_XLINK,
    attrName: "href",
    type: "TrustedScriptURL",
    sink: "SVGScriptElement href",
  },
  // Below are some cases that are not trusted type sinks.
  // event handler attribute name with element in non-HTML/SVG/MathML namespace.
  {
    element: _ => document.createElementNS(NSURI_FOO, "foo"),
    attrNS: null,
    attrName: "onmouseup",
    type: null,
  },
  {
    // event handler attribute name with non-null namespace.
    element: _ => document.createElement("div"),
    attrNS: NSURI_FOO,
    attrName: "onclick",
    type: null,
  },
  {
    // unknown event handler attribute name.
    element: _ => document.createElement("div"),
    attrNS: null,
    attrName: "ondoesnotexist",
    type: null,
  },
  {
    // div attribute that is not protected.
    element: _ => document.createElement("div"),
    attrNS: null,
    attrName: "data-onclick",
    type: null,
  },
  {
    // srcdoc with element's local name that is not iframe.
    element: _ => document.createElement("div"),
    attrNS: null,
    attrName: "srcdoc",
    type: null,
  },
  {
    // srcdoc with element's namespace that is not null.
    element: _ => document.createElementNS(NSURI_FOO, "iframe"),
    attrNS: null,
    attrName: "srcdoc",
    type: null,
  },
  {
    // srcdoc with non-null namespace.
    element: _ => document.createElement("iframe"),
    attrNS: NSURI_FOO,
    attrName: "srcdoc",
    type: null,
  },
  {
    // iframe attribute name that is not protected.
    element: _ => document.createElement("iframe"),
    attrNS: null,
    attrName: "data-srcdoc",
    type: null,
  },
  {
    // src with element's local name that is not script.
    element: _ => document.createElement("div"),
    attrNS: null,
    attrName: "src",
    type: null,
  },
  {
    // src with element's namespace that is not null.
    element: _ => document.createElementNS(NSURI_FOO, "script"),
    attrNS: null,
    attrName: "src",
    type: null,
  },
  {
    // src with non-null namespace.
    element: _ => document.createElement("script"),
    attrNS: NSURI_FOO,
    attrName: "src",
    type: null,
  },
  {
    // script attribute name that is not protected.
    element: _ => document.createElement("script"),
    attrNS: null,
    attrName: "data-src",
    type: null,
  },
  {
    // href with element's local name that is not script.
    element: _ => document.createElementNS(NSURI_SVG, "g"),
    attrNS: null,
    attrName: "href",
    type: null,
  },
  {
    // href with element's namespace that is not SVG.
    element: _ => document.createElement("script"),
    attrNS: null,
    attrName: "href",
    type: null,
  },
  {
    // href with namespace that is neither null nor xlink.
    element: _ => document.createElementNS(NSURI_SVG, "script"),
    attrNS: NSURI_FOO,
    attrName: "href",
    type: null,
  },
  {
    // unknown svg script attribute name.
    element: _ => document.createElementNS(NSURI_SVG, "script"),
    attrNS: null,
    attrName: "src",
    type: null,
  },
];

function findAttribute(element, attrNS, attrName) {
  for (let i = 0; i < element.attributes.length; i++) {
    let attr = element.attributes[i];
    if (attr.namespaceURI === attrNS &&
        attr.localName === attrName) {
      return attr;
    }
  }
}

// This is an array of DOM APIs allowing to set attribute values, described as
// objects with the following members:
// - api: the name of the API (e.g "Element.setAttribute").
// - acceptNS: Whether a attribute namespace can be specified as a parameter to
//   this API (e.g. setAttributeNS) or if it always works on the null namespace
//   (e.g. setAttribute).
// - acceptTrustedTypeArgumentInIDL: Whether the IDL of the API accepts a
//   Trusted Type (https://w3c.github.io/trusted-types/dist/spec/#integrations)
//   as a parameter (e.g. setAttribute) or if it will otherwise just convert
//   such a parameter to a string (e.g. setAttributeNode).
// - runSetter: a function that runs the API to set the element's attribute to
//   the specified value. Before being executed, it may need to create an
//   attribute node. The attribute node before execution (existing or created)
//   is saved on runSetter.lastAttributeNode.
const attributeSetterData = [
  {
    api: "Element.setAttribute",
    acceptNS: false,
    acceptTrustedTypeArgumentInIDL: true,
    runSetter: function(element, attrNS, attrName, attrValue) {
      assert_equals(attrNS, null);
      this.lastAttributeNode = findAttribute(element, attrNS, attrName);
      return element.setAttribute(attrName, attrValue);
    },
  },
  {
    api: "Element.setAttributeNS",
    acceptNS: true,
    acceptTrustedTypeArgumentInIDL: true,
    runSetter: function(element, attrNS, attrName, attrValue) {
      this.lastAttributeNode = findAttribute(element, attrNS, attrName);
      return element.setAttributeNS(attrNS, attrName, attrValue);
    },
  },
  {
    api: "Element.setAttributeNode",
    acceptNS: true,
    acceptTrustedTypeArgumentInIDL: false,
    setterClass: "setAttributeNode",
    runSetter: function(element, attrNS, attrName, attrValue, type) {
      this.lastAttributeNode = document.createAttributeNS(attrNS, attrName);
      this.lastAttributeNode.value = attrValue;
      return element.setAttributeNode(this.lastAttributeNode);
    },
  },
  {
    api: "Element.setAttributeNodeNS",
    acceptNS: true,
    acceptTrustedTypeArgumentInIDL: false,
    runSetter: function(element, attrNS, attrName, attrValue, type) {
      this.lastAttributeNode = document.createAttributeNS(attrNS, attrName);
      this.lastAttributeNode.value = attrValue;
      return element.setAttributeNodeNS(this.lastAttributeNode);
    },
  },
  {
    api: "NamedNodeMap.setNamedItem",
    acceptNS: true,
    acceptTrustedTypeArgumentInIDL: false,
    runSetter: function(element, attrNS, attrName, attrValue, type) {
      const nodeMap = element.attributes;
      this.lastAttributeNode = document.createAttributeNS(attrNS, attrName);
      this.lastAttributeNode.value = attrValue;
      return nodeMap.setNamedItem(this.lastAttributeNode);
    },
  },
  {
    api: "NamedNodeMap.setNamedItemNS",
    acceptNS: true,
    acceptTrustedTypeArgumentInIDL: false,
    runSetter: function(element, attrNS, attrName, attrValue, type) {
      const nodeMap = element.attributes;
      this.lastAttributeNode = document.createAttributeNS(attrNS, attrName);
      this.lastAttributeNode.value = attrValue;
      return nodeMap.setNamedItemNS(this.lastAttributeNode);
    },
  },
  {
    api:"Attr.value",
    acceptNS: true,
      acceptTrustedTypeArgumentInIDL: false,
      runSetter: function(element, attrNS, attrName, attrValue, type) {
      element.setAttributeNS(attrNS, attrName, createTrustedOutput(type, ""));
      this.lastAttributeNode = findAttribute(element, attrNS, attrName);
      assert_true(!!this.lastAttributeNode);
      return (this.lastAttributeNode.value = attrValue);
    },
  },
  {
    api: "Node.nodeValue",
    acceptNS: true,
    acceptTrustedTypeArgumentInIDL: false,
    runSetter: function(element, attrNS, attrName, attrValue, type) {
      element.setAttributeNS(attrNS, attrName, createTrustedOutput(type, ""));
      this.lastAttributeNode = findAttribute(element, attrNS, attrName);
      assert_true(!!this.lastAttributeNode);
      return (this.lastAttributeNode.nodeValue = attrValue);
    },
  },
  {
    api: "Node.textContent",
    acceptNS: true,
    acceptTrustedTypeArgumentInIDL: false,
    runSetter: function(element, attrNS, attrName, attrValue, type) {
      element.setAttributeNS(attrNS, attrName, createTrustedOutput(type, ""));
      this.lastAttributeNode = findAttribute(element, attrNS, attrName);
      assert_true(!!this.lastAttributeNode);
      return (this.lastAttributeNode.textContent = attrValue);
    },
  },
];
