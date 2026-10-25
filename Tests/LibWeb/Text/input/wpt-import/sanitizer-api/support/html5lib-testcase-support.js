// This library supports HTML5lib-style test cases.
//
// The HTMLlib test case format describes an actual DOM tree. For testing, and
// particular for testing of DOM parsers and DOM parser-related functionality,
// this has the advantage of being able to represent edge cases.
//
// Example: If `.replaceWithChildren` is called on the `<span>` element as a
// result of parsing `"<p>Hello<span>World</span></p>"`, then this results in
// a tree with two adjacent text nodes. This behaviour will affect subsequent
// DOM operations and should thus be tested. The HTML5lib format makes it easy
// to describe the expected result unambiguously.
//
// References:
// - HTML5lib: https://github.com/html5lib
// - HTML5lib testcases: https://github.com/html5lib/html5lib-tests/tree/master/tree-construction
// - test case format description:
// https://github.com/html5lib/html5lib-tests/blob/master/tree-construction/README.md
//
// The main "API" is:
//
// - parse_html5lib_testcases(string)
//   This returns an array of dictionaries, where the dictionary contains the
//   the text of the test file, keyed by the lines starting with a hashtag.
//
//   E.g. #data\nbla results in [{data: "bla"}].
//
// - html5lib_testcases_from_script()
//   Wrapper for parse_html5lib_testcases that gets the test data from a script
//   element with type "html5lib-tests". This allows to specify the test data
//   in the test file, but requires working around closing script tags.
//
// - html5lib_testcases_from_response(response_promise)
//   Wrapper for parse_html5lib_testcases that gets the data from a Response
//   Promise, as is returned from `fetch()`, and returns a Promise for the array
//   of testcases. This allows getting the test dat from a text resource.
//
// - build_node_tree(node, documentstr)
//   This builds a node tree from the "#document" string from a testcase, and
//   appends it to the node argument. Returns node.
//
// - assert_subtree_equals(node1, node2)
//   Asserts that the child trees of node1 and node2 are equals. This
//   recursively descends the trees.
//
// - assert_testcase(node, testcase)
//   Wrapper for build_node_tree and assert_subtree_equals, for use with a
//   result of parse_html5lib_testcases.
//

function html5lib_testcases_from_script() {
  return parse_html5lib_testcases(
      document.querySelector("script[type='html5lib-tests']").textContent);
}

function html5lib_testcases_from_response(response_promise) {
  return response_promise
      .then(response => response.text())
      .then(parse_html5lib_testcases);
}

function add_html5lib_testcase(testcases, current) {
  for (const item in current) {
    current[item] = current[item].join("\n");
  }
  if (Object.entries(current).length) {
    testcases.push(current);
  }
}

function parse_html5lib_testcases(content) {
  const testcases = [];
  var state = undefined;
  var current = {};
  for (const line of content.split("\n")) {
    if (!line) {
      add_html5lib_testcase(testcases, current);
      state = undefined;
      current = {};
    } else if (line[0] == "#") {
      state = line.substring(1);
      current[state] = [];
    } else if (state) {
      current[state].push(line);
    } else {
      // Error handling is for another day.
    }
  }
  // Flush the last testcase if the content doesn't end with an empty line.
  add_html5lib_testcase(testcases, current);
  return testcases;
}

function get_child_at(node, level) {
  for (i = 0; i < level; i++) {
    if (is_html_template(node)) {
      // For <template>, continue with the content fragment.
      node = node.content;
    } else {
      node = node.lastChild;
    }
  }
  return node;
}

function append_child_at(node, level, child) {
  get_child_at(node, level).appendChild(child);
}

function is_element(node) {
  return node.tagName && node.namespaceURI;
}

function is_html_template(node) {
  return is_element(node) && node.tagName == "TEMPLATE" &&
      node.namespaceURI == "http://www.w3.org/1999/xhtml";
}

function create_element(name, maybe_namespace) {
  // `For the HTML namespace, the namespace designator is the empty string,
  // i.e. there's no prefix. For the SVG namespace, the namespace designator is
  // "svg ". For the MathML namespace, the namespace designator is "math ".
  if (maybe_namespace == "svg ") {
    return document.createElementNS("http://www.w3.org/2000/svg", name);
  } else if (maybe_namespace == "math ") {
    return document.createElementNS("http://www.w3.org/1998/Math/MathML", name);
  } else if (!maybe_namespace) {
    return document.createElement(name);
  } else {
    assert_unreached(`Invalid element name: "${maybe_namespace}${name}"`);
  }
}

function set_attribute(node, name, maybe_namespace, value) {
  // `The attribute name string is the local name prefixed by a namespace
  // designator. For no namespace, the namespace designator is the empty string,
  // i.e. there's no prefix. For the XLink namespace, the namespace designator
  // is "xlink " [Likewise, "xml " and "xmlns ".]
  if (maybe_namespace == "xlink ") {
    node.setAttributeNS("http://www.w3.org/1999/xlink", name, value);
  } else if (maybe_namespace == "xml ") {
    node.setAttributeNS("http://www.w3.org/XML/1998/namespace", name, value);
  } else if (maybe_namespace == "xmlns ") {
    node.setAttributeNS("http://www.w3.org/2000/xmlns/", name, value);
  } else if (!maybe_namespace) {
    node.setAttribute(name, value);
  } else {
    assert_unreached(`Invalid attribute name: "${maybe_namespace}${name}"`);
  }
}

function build_node_tree(root, docstr) {
  // Format described here:
  // https://github.com/html5lib/html5lib-tests/blob/master/tree-construction/README.md

  // Special-case empty string: Don't build anything.
  // (Happens for test docs that cause parse errors, but also for genuinely
  // empty expectation documents.)
  if (!docstr) return root;

  for (const line of docstr.split("\n")) {
    const [_, indent, remainder] = line.match(/^\| ( *)(.*)/);
    const level = indent.length / 2;
    if (match = remainder.match(/^<([a-z]* )?([a-zA-Z0-9_-]*)>$/)) {
      // `Element nodes must be represented by a "<, the tag name string, ">".`
      append_child_at(root, level, create_element(match[2], match[1]));
    } else if (match = remainder.match(/^"([^"]*)"$/)) {
      // `Text nodes must be the string, in double quotes.`
      append_child_at(root, level, document.createTextNode(match[1]));
    } else if (match = remainder.match(/^([a-z]* )?(.*)="(.*)"$/)) {
      // `Attribute nodes must have the attribute name string, then an "=" sign,
      // then the attribute value in double quotes (").`
      set_attribute(get_child_at(root, level), match[2], match[1], match[3]);
    } else if (match = remainder.match(/^<!--(.*)-->$/)) {
      // `Comments must be "<" then "!-- " then the data then " -->".`
      append_child_at(root, level, document.createComment(match[1]));
    } else if (match = remainder.match(
        /^<!DOCTYPE ([^ ]*)( "([^"]*)"( "([^"]*)")?)?>$/)) {
      // `DOCTYPEs must be "<!DOCTYPE " then [... bla bla ...]`
      append_child_at(root, level,
         document.implementation.createDocumentType(match[1], match[3], match[5]));
    } else if (match = remainder.match(/^<\?([a-zA-Z0-9_-]*)(?:\s+(.*?))?\?>$/)) {
      // `Processing instructions must be "<?", then the target, then [...], ending in "?>"`
      append_child_at(root, level, document.createProcessingInstruction(
          match[1], match[2] || ""));
    } else if (remainder == "content") {
      // Template contents are represented by the string "content" with the
      // children below it.
      // Nothing to do here; so let's just check we're actually in a template.
      assert_true(is_html_template(get_child_at(root, level)),
          "\"content\" only expected as child of a <template>.");
    } else {
      assert_unreached(
          `Unknown line type. Maybe test data is malformed. ("${line}")`);
    }
  }
  return root;
}

function assert_subtree_equals(node1, node2) {
  // Iterate in parallel over both trees.
  const tree1 = document.createNodeIterator(node1);
  const tree2 = document.createNodeIterator(node2);
  // Skip the root/context node, so that we can re-use the test with different
  // context types.
  var current1 = tree1.nextNode();
  var current2 = tree2.nextNode();
  do {
    current1 = tree1.nextNode();
    current2 = tree2.nextNode();

    if (!current1) break;

    // Conceptually, we only want to check whether a.isEqualNode(b). But that
    // yields terrible error messages ("expected true but got false"). With
    // this being a test suite and all, let's invest a bit of effort into nice
    // error messages.
    if (!current1.isEqualNode(current2)) {
      let breadcrumbs = "";
      let current = current1;
      while (current) {
        const here = is_element(current) ? `<${current.tagName}>` : `${current}`;
        breadcrumbs = `${here} / ${breadcrumbs}`;
        current = current.parentNode;
      }
      breadcrumbs = breadcrumbs.substring(0, breadcrumbs.length - 3);
      assert_true(current1.isEqualNode(current2),
          `${current1}.isEqual(${current2}) fails. Path: ${breadcrumbs}.`);
    }

    // NodeIterator does not recurse into template contents. So we need to do
    // this manually.
    if (is_html_template(current1) && is_html_template(current2)) {
      assert_subtree_equals(current1.content, current2.content);
    }
  } while (current1);

  // Ensure that both iterators have come to an end.
  assert_false(!!current2, "Additional nodes at the of node2.\n");
}

function assert_testcase(node, testcase) {
  const context = document.createElement(testcase["document-fragment"] ?? "div");
  const tree = build_node_tree(context, testcase.document);
  assert_subtree_equals(node, tree);
}
