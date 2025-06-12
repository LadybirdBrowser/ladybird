// Require selectors.js to be included before this.

/*
 * Create and append special elements that cannot be created correctly with HTML markup alone.
 */
function setupSpecialElements(doc, parent) {
  // Setup null and undefined tests
  parent.appendChild(doc.createElement("null"));
  parent.appendChild(doc.createElement("undefined"));

  // Setup namespace tests
  var anyNS = doc.createElement("div");
  var noNS = doc.createElement("div");
  anyNS.id = "any-namespace";
  noNS.id = "no-namespace";

  var divs;
  div = [doc.createElement("div"),
         doc.createElementNS("http://www.w3.org/1999/xhtml", "div"),
         doc.createElementNS("", "div"),
         doc.createElementNS("http://www.example.org/ns", "div")];

  div[0].id = "any-namespace-div1";
  div[1].id = "any-namespace-div2";
  div[2].setAttribute("id", "any-namespace-div3"); // Non-HTML elements can't use .id property
  div[3].setAttribute("id", "any-namespace-div4");

  for (var i = 0; i < div.length; i++) {
    anyNS.appendChild(div[i])
  }

  div = [doc.createElement("div"),
         doc.createElementNS("http://www.w3.org/1999/xhtml", "div"),
         doc.createElementNS("", "div"),
         doc.createElementNS("http://www.example.org/ns", "div")];

  div[0].id = "no-namespace-div1";
  div[1].id = "no-namespace-div2";
  div[2].setAttribute("id", "no-namespace-div3"); // Non-HTML elements can't use .id property
  div[3].setAttribute("id", "no-namespace-div4");

  for (i = 0; i < div.length; i++) {
    noNS.appendChild(div[i])
  }

  parent.appendChild(anyNS);
  parent.appendChild(noNS);

  var span = doc.getElementById("attr-presence-i1");
  span.setAttributeNS("http://www.example.org/ns", "title", "");
}

/*
 * Check that the querySelector and querySelectorAll methods exist on the given Node
 */
function interfaceCheck(type, obj) {
  test(function() {
    var q = typeof obj.querySelector === "function";
    assert_true(q, type + " supports querySelector.");
  }, type + " supports querySelector")

  test(function() {
    var qa = typeof obj.querySelectorAll === "function";
    assert_true( qa, type + " supports querySelectorAll.");
  }, type + " supports querySelectorAll")

  test(function() {
    var list = obj.querySelectorAll("div");
    if (obj.ownerDocument) { // The object is not a Document
      assert_true(list instanceof obj.ownerDocument.defaultView.NodeList, "The result should be an instance of a NodeList")
    } else { // The object is a Document
      assert_true(list instanceof obj.defaultView.NodeList, "The result should be an instance of a NodeList")
    }
  }, type + ".querySelectorAll returns NodeList instance")
}

/*
 * Verify that the NodeList returned by querySelectorAll is static and and that a new list is created after
 * each call. A static list should not be affected by subsequent changes to the DOM.
 */
function verifyStaticList(type, doc, root) {
  var pre, post, preLength;

  test(function() {
    pre = root.querySelectorAll("div");
    preLength = pre.length;

    var div = doc.createElement("div");
    (root.body || root).appendChild(div);

    assert_equals(pre.length, preLength, "The length of the NodeList should not change.")
  }, type + ": static NodeList")

  test(function() {
    post = root.querySelectorAll("div"),
    assert_equals(post.length, preLength + 1, "The length of the new NodeList should be 1 more than the previous list.")
  }, type + ": new NodeList")
}

/*
 * Verify handling of special values for the selector parameter, including stringification of
 * null and undefined, and the handling of the empty string.
 */
function runSpecialSelectorTests(type, root) {
  let global = (root.ownerDocument || root).defaultView;

  test(function() { // 1
    assert_equals(root.querySelectorAll(null).length, 1, "This should find one element with the tag name 'NULL'.");
  }, type + ".querySelectorAll null")

  test(function() { // 2
    assert_equals(root.querySelectorAll(undefined).length, 1, "This should find one element with the tag name 'UNDEFINED'.");
  }, type + ".querySelectorAll undefined")

  test(function() { // 3
    assert_throws_js(global.TypeError, function() {
      root.querySelectorAll();
    }, "This should throw a TypeError.")
  }, type + ".querySelectorAll no parameter")

  test(function() { // 4
    var elm = root.querySelector(null)
    assert_not_equals(elm, null, "This should find an element.");
    assert_equals(elm.tagName.toUpperCase(), "NULL", "The tag name should be 'NULL'.")
  }, type + ".querySelector null")

  test(function() { // 5
    var elm = root.querySelector(undefined)
    assert_not_equals(elm, undefined, "This should find an element.");
    assert_equals(elm.tagName.toUpperCase(), "UNDEFINED", "The tag name should be 'UNDEFINED'.")
  }, type + ".querySelector undefined")

  test(function() { // 6
    assert_throws_js(global.TypeError, function() {
      root.querySelector();
    }, "This should throw a TypeError.")
  }, type + ".querySelector no parameter")

  test(function() { // 7
    result = root.querySelectorAll("*");
    var i = 0;
    traverse(root, function(elem) {
      if (elem !== root) {
        assert_equals(elem, result[i], "The result in index " + i + " should be in tree order.");
        i++;
      }
    })
  }, type + ".querySelectorAll tree order");
}

/*
 * Execute queries with the specified valid selectors for both querySelector() and querySelectorAll()
 * Only run these tests when results are expected. Don't run for syntax error tests.
 */
function runValidSelectorTest(type, root, selectors, testType, docType) {
  var nodeType = "";
  switch (root.nodeType) {
    case Node.DOCUMENT_NODE:
      nodeType = "document";
      break;
    case Node.ELEMENT_NODE:
      nodeType = root.parentNode ? "element" : "detached";
      break;
    case Node.DOCUMENT_FRAGMENT_NODE:
      nodeType = "fragment";
      break;
    default:
      assert_unreached();
      nodeType = "unknown"; // This should never happen.
  }

  for (var i = 0; i < selectors.length; i++) {
    var s = selectors[i];
    var n = s["name"];
    var q = s["selector"];
    var e = s["expect"];

    if ((!s["exclude"] || (s["exclude"].indexOf(nodeType) === -1 && s["exclude"].indexOf(docType) === -1))
     && (s["testType"] & testType) ) {
      var foundall, found;

      test(function() {
        foundall = root.querySelectorAll(q);
        assert_not_equals(foundall, null, "The method should not return null.")
        assert_equals(foundall.length, e.length, "The method should return the expected number of matches.")

        for (var i = 0; i < e.length; i++) {
          assert_not_equals(foundall[i], null, "The item in index " + i + " should not be null.")
          assert_equals(foundall[i].getAttribute("id"), e[i], "The item in index " + i + " should have the expected ID.");
          assert_false(foundall[i].hasAttribute("data-clone"), "This should not be a cloned element.");
        }
      }, type + ".querySelectorAll: " + n + ": " + q);

      test(function() {
        found = root.querySelector(q);

        if (e.length > 0) {
          assert_not_equals(found, null, "The method should return a match.")
          assert_equals(found.getAttribute("id"), e[0], "The method should return the first match.");
          assert_equals(found, foundall[0], "The result should match the first item from querySelectorAll.");
          assert_false(found.hasAttribute("data-clone"), "This should not be annotated as a cloned element.");
        } else {
          assert_equals(found, null, "The method should not match anything.");
        }
      }, type + ".querySelector: " + n + ": " + q);
    }
  }
}

function windowFor(root) {
  return root.defaultView || root.ownerDocument.defaultView;
}

/*
 * Execute queries with the specified invalid selectors for both querySelector() and querySelectorAll()
 * Only run these tests when errors are expected. Don't run for valid selector tests.
 */
function runInvalidSelectorTest(type, root, selectors) {
  for (var i = 0; i < selectors.length; i++) {
    var s = selectors[i];
    var n = s["name"];
    var q = s["selector"];

    test(function() {
      assert_throws_dom("SyntaxError", windowFor(root).DOMException, function() {
        root.querySelector(q)
      });
    }, type + ".querySelector: " + n + ": " + q);

    test(function() {
      assert_throws_dom("SyntaxError", windowFor(root).DOMException, function() {
        root.querySelectorAll(q)
      });
    }, type + ".querySelectorAll: " + n + ": " + q);
  }
}

function traverse(elem, fn) {
  if (elem.nodeType === elem.ELEMENT_NODE) {
    fn(elem);
  }
  elem = elem.firstChild;
  while (elem) {
    traverse(elem, fn);
    elem = elem.nextSibling;
  }
}

function getNodeType(node) {
  switch (node.nodeType) {
    case Node.DOCUMENT_NODE:
      return "document";
    case Node.ELEMENT_NODE:
      return node.parentNode ? "element" : "detached";
    case Node.DOCUMENT_FRAGMENT_NODE:
      return "fragment";
    default:
      assert_unreached();
      return "unknown"; // This should never happen.
  }
}
