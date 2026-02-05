function getNonParentNodes() {
  return [
    document.implementation.createDocumentType("html", "", ""),
    document.createTextNode("text"),
    document.implementation.createDocument(null, "foo", null).createProcessingInstruction("foo", "bar"),
    document.createComment("comment"),
    document.implementation.createDocument(null, "foo", null).createCDATASection("data"),
  ];
}

function getNonInsertableNodes() {
  return [
    document.implementation.createHTMLDocument("title")
  ];
}

function getNonDocumentParentNodes() {
  return [
    document.createElement("div"),
    document.createDocumentFragment(),
  ];
}

// Test that the steps happen in the right order, to the extent that it's
// observable.   The variable names "parent", "child", and "node" match the
// corresponding variables in the replaceChild algorithm in these tests.

// Step 1 happens before step 3.
test(function() {
  var illegalParents = getNonParentNodes();
  var child = document.createElement("div");
  var node = document.createElement("div");
  illegalParents.forEach(function (parent) {
    assert_throws_dom("HierarchyRequestError", function() {
      insertFunc.call(parent, node, child);
    });
  });
}, "Should check the 'parent' type before checking whether 'child' is a child of 'parent'");

// Step 2 happens before step 3.
test(function() {
  var parent = document.createElement("div");
  var child = document.createElement("div");
  var node = document.createElement("div");

  node.appendChild(parent);
  assert_throws_dom("HierarchyRequestError", function() {
    insertFunc.call(parent, node, child);
  });
}, "Should check that 'node' is not an ancestor of 'parent' before checking whether 'child' is a child of 'parent'");

// Step 3 happens before step 4.
test(function() {
  var parent = document.createElement("div");
  var child = document.createElement("div");

  var illegalChildren = getNonInsertableNodes();
  illegalChildren.forEach(function (node) {
    assert_throws_dom("NotFoundError", function() {
      insertFunc.call(parent, node, child);
    });
  });
}, "Should check whether 'child' is a child of 'parent' before checking whether 'node' is of a type that can have a parent.");


// Step 3 happens before step 5.
test(function() {
  var child = document.createElement("div");

  var node = document.createTextNode("");
  var parent = document.implementation.createDocument(null, "foo", null);
  assert_throws_dom("NotFoundError", function() {
    insertFunc.call(parent, node, child);
  });

  node = document.implementation.createDocumentType("html", "", "");
  getNonDocumentParentNodes().forEach(function (parent) {
    assert_throws_dom("NotFoundError", function() {
      insertFunc.call(parent, node, child);
    });
  });
}, "Should check whether 'child' is a child of 'parent' before checking whether 'node' is of a type that can have a parent of the type that 'parent' is.");

// Step 3 happens before step 6.
test(function() {
  var child = document.createElement("div");
  var parent = document.implementation.createDocument(null, null, null);

  var node = document.createDocumentFragment();
  node.appendChild(document.createElement("div"));
  node.appendChild(document.createElement("div"));
  assert_throws_dom("NotFoundError", function() {
    insertFunc.call(parent, node, child);
  });

  node = document.createElement("div");
  parent.appendChild(document.createElement("div"));
  assert_throws_dom("NotFoundError", function() {
    insertFunc.call(parent, node, child);
  });

  parent.firstChild.remove();
  parent.appendChild(document.implementation.createDocumentType("html", "", ""));
  node = document.implementation.createDocumentType("html", "", "")
  assert_throws_dom("NotFoundError", function() {
    insertFunc.call(parent, node, child);
  });
}, "Should check whether 'child' is a child of 'parent' before checking whether 'node' can be inserted into the document given the kids the document has right now.");
