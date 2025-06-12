"use strict";

// These tests probably use too much abstraction and too little copy-paste.
// Reader beware.
//
// TODO:
//
// * Lots and lots and lots more different types of ranges
// * insertBefore() with DocumentFragments
// * Fill out other insert/remove tests
// * normalize() (https://www.w3.org/Bugs/Public/show_bug.cgi?id=13843)

// Give a textual description of the range we're testing, for the test names.
function describeRange(startContainer, startOffset, endContainer, endOffset) {
  if (startContainer == endContainer && startOffset == endOffset) {
    return "range collapsed at (" + startContainer + ", " + startOffset + ")";
  } else if (startContainer == endContainer) {
    return "range on " + startContainer + " from " + startOffset + " to " + endOffset;
  } else {
    return "range from (" + startContainer + ", " + startOffset + ") to (" + endContainer + ", " + endOffset + ")";
  }
}

// Lists of the various types of nodes we'll want to use.  We use strings that
// we can later eval(), so that we can produce legible test names.
var textNodes = [
  "paras[0].firstChild",
  "paras[1].firstChild",
  "foreignTextNode",
  "xmlTextNode",
  "detachedTextNode",
  "detachedForeignTextNode",
  "detachedXmlTextNode",
];
var commentNodes = [
  "comment",
  "foreignComment",
  "xmlComment",
  "detachedComment",
  "detachedForeignComment",
  "detachedXmlComment",
];
var characterDataNodes = textNodes.concat(commentNodes);

// This function is slightly scary, but it works well enough, so . . .
// sourceTests is an array of test data that will be altered in mysterious ways
// before being passed off to doTest, descFn is something that takes an element
// of sourceTests and produces the first part of a human-readable description
// of the test, testFn is the function that doTest will call to do the actual
// work and tell it what results to expect.
function doTests(sourceTests, descFn, testFn) {
  var tests = [];
  for (var i = 0; i < sourceTests.length; i++) {
    var params = sourceTests[i];
    var len = params.length;
    tests.push([
      descFn(params) + ", with unselected " + describeRange(params[len - 4], params[len - 3], params[len - 2], params[len - 1]),
      // The closure here ensures that the params that testFn get are the
      // current version of params, not the version from the last
      // iteration of this loop.  We test that none of the parameters
      // evaluate to undefined to catch bugs in our eval'ing, like
      // mistyping a property name.
      function(params) { return function() {
        var evaledParams = params.map(eval);
        for (var i = 0; i < evaledParams.length; i++) {
          assert_not_equals(typeof evaledParams[i], "undefined",
            "Test bug: " + params[i] + " is undefined");
        }
        return testFn.apply(null, evaledParams);
      } }(params),
      false,
      params[len - 4],
      params[len - 3],
      params[len - 2],
      params[len - 1]
    ]);
    tests.push([
      descFn(params) + ", with selected " + describeRange(params[len - 4], params[len - 3], params[len - 2], params[len - 1]),
      function(params) { return function(selectedRange) {
        var evaledParams = params.slice(0, len - 4).map(eval);
        for (var i = 0; i < evaledParams.length; i++) {
          assert_not_equals(typeof evaledParams[i], "undefined",
            "Test bug: " + params[i] + " is undefined");
        }
        // Override input range with the one that was actually selected when computing the expected result.
        evaledParams = evaledParams.concat([selectedRange.startContainer, selectedRange.startOffset, selectedRange.endContainer, selectedRange.endOffset]);
        return testFn.apply(null, evaledParams);
      } }(params),
      true,
      params[len - 4],
      params[len - 3],
      params[len - 2],
      params[len - 1]
    ]);
  }
  generate_tests(doTest, tests);
}

// Set up the range, call the callback function to do the DOM modification and
// tell us what to expect.  The callback function needs to return a
// four-element array with the expected start/end containers/offsets, and
// receives no arguments.  useSelection tells us whether the Range should be
// added to a Selection and the Selection tested to ensure that the mutation
// affects user selections as well as other ranges; every test is run with this
// both false and true, because when it's set to true WebKit and Opera fail all
// tests' sanity checks, which is unhelpful.  The last four parameters just
// tell us what range to build.
function doTest(callback, useSelection, startContainer, startOffset, endContainer, endOffset) {
  // Recreate all the test nodes in case they were altered by the last test
  // run.
  setupRangeTests();
  startContainer = eval(startContainer);
  startOffset = eval(startOffset);
  endContainer = eval(endContainer);
  endOffset = eval(endOffset);

  var ownerDoc = startContainer.nodeType == Node.DOCUMENT_NODE
    ? startContainer
    : startContainer.ownerDocument;
  var range = ownerDoc.createRange();
  range.setStart(startContainer, startOffset);
  range.setEnd(endContainer, endOffset);

  if (useSelection) {
    getSelection().removeAllRanges();
    getSelection().addRange(range);

    // Some browsers refuse to add a range unless it results in an actual visible selection.
    if (!getSelection().rangeCount)
        return;

    // Override range with the one that was actually selected as it differs in some browsers.
    range = getSelection().getRangeAt(0);
  }

  var expected = callback(range);

  assert_equals(range.startContainer, expected[0],
    "Wrong start container");
  assert_equals(range.startOffset, expected[1],
    "Wrong start offset");
  assert_equals(range.endContainer, expected[2],
    "Wrong end container");
  assert_equals(range.endOffset, expected[3],
    "Wrong end offset");
}


// Now we get to the specific tests.

function testSplitText(oldNode, offset, startContainer, startOffset, endContainer, endOffset) {
  // Save these for later
  var originalStartOffset = startOffset;
  var originalEndOffset = endOffset;
  var originalLength = oldNode.length;

  var newNode;
  try {
    newNode = oldNode.splitText(offset);
  } catch (e) {
    // Should only happen if offset is negative
    return [startContainer, startOffset, endContainer, endOffset];
  }

  // First we adjust for replacing data:
  //
  // "Replace data with offset offset, count count, and data the empty
  // string."
  //
  // That translates to offset = offset, count = originalLength - offset,
  // data = "".  node is oldNode.
  //
  // "For every boundary point whose node is node, and whose offset is
  // greater than offset but less than or equal to offset plus count, set its
  // offset to offset."
  if (startContainer == oldNode
  && startOffset > offset
  && startOffset <= originalLength) {
    startOffset = offset;
  }

  if (endContainer == oldNode
  && endOffset > offset
  && endOffset <= originalLength) {
    endOffset = offset;
  }

  // "For every boundary point whose node is node, and whose offset is
  // greater than offset plus count, add the length of data to its offset,
  // then subtract count from it."
  //
  // Can't happen: offset plus count is originalLength.

  // Now we insert a node, if oldNode's parent isn't null: "For each boundary
  // point whose node is the new parent of the affected node and whose offset
  // is greater than the new index of the affected node, add one to the
  // boundary point's offset."
  if (startContainer == oldNode.parentNode
  && startOffset > 1 + indexOf(oldNode)) {
    startOffset++;
  }

  if (endContainer == oldNode.parentNode
  && endOffset > 1 + indexOf(oldNode)) {
    endOffset++;
  }

  // Finally, the splitText stuff itself:
  //
  // "If parent is not null, run these substeps:
  //
  //   * "For each range whose start node is node and start offset is greater
  //   than offset, set its start node to new node and decrease its start
  //   offset by offset.
  //
  //   * "For each range whose end node is node and end offset is greater
  //   than offset, set its end node to new node and decrease its end offset
  //   by offset.
  //
  //   * "For each range whose start node is parent and start offset is equal
  //   to the index of node + 1, increase its start offset by one.
  //
  //   * "For each range whose end node is parent and end offset is equal to
  //   the index of node + 1, increase its end offset by one."
  if (oldNode.parentNode) {
    if (startContainer == oldNode && originalStartOffset > offset) {
      startContainer = newNode;
      startOffset = originalStartOffset - offset;
    }

    if (endContainer == oldNode && originalEndOffset > offset) {
      endContainer = newNode;
      endOffset = originalEndOffset - offset;
    }

    if (startContainer == oldNode.parentNode
    && startOffset == 1 + indexOf(oldNode)) {
      startOffset++;
    }

    if (endContainer == oldNode.parentNode
    && endOffset == 1 + indexOf(oldNode)) {
      endOffset++;
    }
  }

  return [startContainer, startOffset, endContainer, endOffset];
}

// The offset argument is unsigned, so per WebIDL -1 should wrap to 4294967295,
// which is probably longer than the length, so it should throw an exception.
// This is no different from the other cases where the offset is longer than
// the length, and the wrapping complicates my testing slightly, so I won't
// bother testing negative values here or in other cases.
var splitTextTests = [];
for (var i = 0; i < textNodes.length; i++) {
  var node = textNodes[i];
  splitTextTests.push([node, 376, node, 0, node, 1]);
  splitTextTests.push([node, 0, node, 0, node, 0]);
  splitTextTests.push([node, 1, node, 1, node, 1]);
  splitTextTests.push([node, node + ".length", node, node + ".length", node, node + ".length"]);
  splitTextTests.push([node, 1, node, 1, node, 3]);
  splitTextTests.push([node, 2, node, 1, node, 3]);
  splitTextTests.push([node, 3, node, 1, node, 3]);
}

splitTextTests.push(
  ["paras[0].firstChild", 1, "paras[0]", 0, "paras[0]", 0],
  ["paras[0].firstChild", 1, "paras[0]", 0, "paras[0]", 1],
  ["paras[0].firstChild", 1, "paras[0]", 1, "paras[0]", 1],
  ["paras[0].firstChild", 1, "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", 2, "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", 3, "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", 1, "paras[0]", 0, "paras[0].firstChild", 3],
  ["paras[0].firstChild", 2, "paras[0]", 0, "paras[0].firstChild", 3],
  ["paras[0].firstChild", 3, "paras[0]", 0, "paras[0].firstChild", 3]
);


function testReplaceDataAlgorithm(node, offset, count, data, callback, startContainer, startOffset, endContainer, endOffset) {
  // Mutation works the same any time DOM Core's "replace data" algorithm is
  // invoked.  node, offset, count, data are as in that algorithm.  The
  // callback is what does the actual setting.  Not to be confused with
  // testReplaceData, which tests the replaceData() method.

  // Barring any provision to the contrary, the containers and offsets must
  // not change.
  var expectedStartContainer = startContainer;
  var expectedStartOffset = startOffset;
  var expectedEndContainer = endContainer;
  var expectedEndOffset = endOffset;

  var originalParent = node.parentNode;
  var originalData = node.data;

  var exceptionThrown = false;
  try {
    callback();
  } catch (e) {
    // Should only happen if offset is greater than length
    exceptionThrown = true;
  }

  assert_equals(node.parentNode, originalParent,
    "Sanity check failed: changing data changed the parent");

  // "User agents must run the following steps whenever they replace data of
  // a CharacterData node, as though they were written in the specification
  // for that algorithm after all other steps. In particular, the steps must
  // not be executed if the algorithm threw an exception."
  if (exceptionThrown) {
    assert_equals(node.data, originalData,
      "Sanity check failed: exception thrown but data changed");
  } else {
    assert_equals(node.data,
      originalData.substr(0, offset) + data + originalData.substr(offset + count),
      "Sanity check failed: data not changed as expected");
  }

  // "For every boundary point whose node is node, and whose offset is
  // greater than offset but less than or equal to offset plus count, set
  // its offset to offset."
  if (!exceptionThrown
  && startContainer == node
  && startOffset > offset
  && startOffset <= offset + count) {
    expectedStartOffset = offset;
  }

  if (!exceptionThrown
  && endContainer == node
  && endOffset > offset
  && endOffset <= offset + count) {
    expectedEndOffset = offset;
  }

  // "For every boundary point whose node is node, and whose offset is
  // greater than offset plus count, add the length of data to its offset,
  // then subtract count from it."
  if (!exceptionThrown
  && startContainer == node
  && startOffset > offset + count) {
    expectedStartOffset += data.length - count;
  }

  if (!exceptionThrown
  && endContainer == node
  && endOffset > offset + count) {
    expectedEndOffset += data.length - count;
  }

  return [expectedStartContainer, expectedStartOffset, expectedEndContainer, expectedEndOffset];
}

function testInsertData(node, offset, data, startContainer, startOffset, endContainer, endOffset) {
  return testReplaceDataAlgorithm(node, offset, 0, data,
    function() { node.insertData(offset, data) },
    startContainer, startOffset, endContainer, endOffset);
}

var insertDataTests = [];
for (var i = 0; i < characterDataNodes.length; i++) {
  var node = characterDataNodes[i];
  insertDataTests.push([node, 376, '"foo"', node, 0, node, 1]);
  insertDataTests.push([node, 0, '"foo"', node, 0, node, 0]);
  insertDataTests.push([node, 1, '"foo"', node, 1, node, 1]);
  insertDataTests.push([node, node + ".length", '"foo"', node, node + ".length", node, node + ".length"]);
  insertDataTests.push([node, 1, '"foo"', node, 1, node, 3]);
  insertDataTests.push([node, 2, '"foo"', node, 1, node, 3]);
  insertDataTests.push([node, 3, '"foo"', node, 1, node, 3]);

  insertDataTests.push([node, 376, '""', node, 0, node, 1]);
  insertDataTests.push([node, 0, '""', node, 0, node, 0]);
  insertDataTests.push([node, 1, '""', node, 1, node, 1]);
  insertDataTests.push([node, node + ".length", '""', node, node + ".length", node, node + ".length"]);
  insertDataTests.push([node, 1, '""', node, 1, node, 3]);
  insertDataTests.push([node, 2, '""', node, 1, node, 3]);
  insertDataTests.push([node, 3, '""', node, 1, node, 3]);
}

insertDataTests.push(
  ["paras[0].firstChild", 1, '"foo"', "paras[0]", 0, "paras[0]", 0],
  ["paras[0].firstChild", 1, '"foo"', "paras[0]", 0, "paras[0]", 1],
  ["paras[0].firstChild", 1, '"foo"', "paras[0]", 1, "paras[0]", 1],
  ["paras[0].firstChild", 1, '"foo"', "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", 2, '"foo"', "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", 3, '"foo"', "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", 1, '"foo"', "paras[0]", 0, "paras[0].firstChild", 3],
  ["paras[0].firstChild", 2, '"foo"', "paras[0]", 0, "paras[0].firstChild", 3],
  ["paras[0].firstChild", 3, '"foo"', "paras[0]", 0, "paras[0].firstChild", 3]
);


function testAppendData(node, data, startContainer, startOffset, endContainer, endOffset) {
  return testReplaceDataAlgorithm(node, node.length, 0, data,
    function() { node.appendData(data) },
    startContainer, startOffset, endContainer, endOffset);
}

var appendDataTests = [];
for (var i = 0; i < characterDataNodes.length; i++) {
  var node = characterDataNodes[i];
  appendDataTests.push([node, '"foo"', node, 0, node, 1]);
  appendDataTests.push([node, '"foo"', node, 0, node, 0]);
  appendDataTests.push([node, '"foo"', node, 1, node, 1]);
  appendDataTests.push([node, '"foo"', node, 0, node, node + ".length"]);
  appendDataTests.push([node, '"foo"', node, 1, node, node + ".length"]);
  appendDataTests.push([node, '"foo"', node, node + ".length", node, node + ".length"]);
  appendDataTests.push([node, '"foo"', node, 1, node, 3]);

  appendDataTests.push([node, '""', node, 0, node, 1]);
  appendDataTests.push([node, '""', node, 0, node, 0]);
  appendDataTests.push([node, '""', node, 1, node, 1]);
  appendDataTests.push([node, '""', node, 0, node, node + ".length"]);
  appendDataTests.push([node, '""', node, 1, node, node + ".length"]);
  appendDataTests.push([node, '""', node, node + ".length", node, node + ".length"]);
  appendDataTests.push([node, '""', node, 1, node, 3]);
}

appendDataTests.push(
  ["paras[0].firstChild", '""', "paras[0]", 0, "paras[0]", 0],
  ["paras[0].firstChild", '""', "paras[0]", 0, "paras[0]", 1],
  ["paras[0].firstChild", '""', "paras[0]", 1, "paras[0]", 1],
  ["paras[0].firstChild", '""', "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", '""', "paras[0]", 0, "paras[0].firstChild", 3],

  ["paras[0].firstChild", '"foo"', "paras[0]", 0, "paras[0]", 0],
  ["paras[0].firstChild", '"foo"', "paras[0]", 0, "paras[0]", 1],
  ["paras[0].firstChild", '"foo"', "paras[0]", 1, "paras[0]", 1],
  ["paras[0].firstChild", '"foo"', "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", '"foo"', "paras[0]", 0, "paras[0].firstChild", 3]
);


function testDeleteData(node, offset, count, startContainer, startOffset, endContainer, endOffset) {
  return testReplaceDataAlgorithm(node, offset, count, "",
    function() { node.deleteData(offset, count) },
    startContainer, startOffset, endContainer, endOffset);
}

var deleteDataTests = [];
for (var i = 0; i < characterDataNodes.length; i++) {
  var node = characterDataNodes[i];
  deleteDataTests.push([node, 376, 2, node, 0, node, 1]);
  deleteDataTests.push([node, 0, 2, node, 0, node, 0]);
  deleteDataTests.push([node, 1, 2, node, 1, node, 1]);
  deleteDataTests.push([node, node + ".length", 2, node, node + ".length", node, node + ".length"]);
  deleteDataTests.push([node, 1, 2, node, 1, node, 3]);
  deleteDataTests.push([node, 2, 2, node, 1, node, 3]);
  deleteDataTests.push([node, 3, 2, node, 1, node, 3]);

  deleteDataTests.push([node, 376, 0, node, 0, node, 1]);
  deleteDataTests.push([node, 0, 0, node, 0, node, 0]);
  deleteDataTests.push([node, 1, 0, node, 1, node, 1]);
  deleteDataTests.push([node, node + ".length", 0, node, node + ".length", node, node + ".length"]);
  deleteDataTests.push([node, 1, 0, node, 1, node, 3]);
  deleteDataTests.push([node, 2, 0, node, 1, node, 3]);
  deleteDataTests.push([node, 3, 0, node, 1, node, 3]);

  deleteDataTests.push([node, 376, 631, node, 0, node, 1]);
  deleteDataTests.push([node, 0, 631, node, 0, node, 0]);
  deleteDataTests.push([node, 1, 631, node, 1, node, 1]);
  deleteDataTests.push([node, node + ".length", 631, node, node + ".length", node, node + ".length"]);
  deleteDataTests.push([node, 1, 631, node, 1, node, 3]);
  deleteDataTests.push([node, 2, 631, node, 1, node, 3]);
  deleteDataTests.push([node, 3, 631, node, 1, node, 3]);
}

deleteDataTests.push(
  ["paras[0].firstChild", 1, 2, "paras[0]", 0, "paras[0]", 0],
  ["paras[0].firstChild", 1, 2, "paras[0]", 0, "paras[0]", 1],
  ["paras[0].firstChild", 1, 2, "paras[0]", 1, "paras[0]", 1],
  ["paras[0].firstChild", 1, 2, "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", 2, 2, "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", 3, 2, "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", 1, 2, "paras[0]", 0, "paras[0].firstChild", 3],
  ["paras[0].firstChild", 2, 2, "paras[0]", 0, "paras[0].firstChild", 3],
  ["paras[0].firstChild", 3, 2, "paras[0]", 0, "paras[0].firstChild", 3]
);


function testReplaceData(node, offset, count, data, startContainer, startOffset, endContainer, endOffset) {
  return testReplaceDataAlgorithm(node, offset, count, data,
    function() { node.replaceData(offset, count, data) },
    startContainer, startOffset, endContainer, endOffset);
}

var replaceDataTests = [];
for (var i = 0; i < characterDataNodes.length; i++) {
  var node = characterDataNodes[i];
  replaceDataTests.push([node, 376, 0, '"foo"', node, 0, node, 1]);
  replaceDataTests.push([node, 0, 0, '"foo"', node, 0, node, 0]);
  replaceDataTests.push([node, 1, 0, '"foo"', node, 1, node, 1]);
  replaceDataTests.push([node, node + ".length", 0, '"foo"', node, node + ".length", node, node + ".length"]);
  replaceDataTests.push([node, 1, 0, '"foo"', node, 1, node, 3]);
  replaceDataTests.push([node, 2, 0, '"foo"', node, 1, node, 3]);
  replaceDataTests.push([node, 3, 0, '"foo"', node, 1, node, 3]);

  replaceDataTests.push([node, 376, 0, '""', node, 0, node, 1]);
  replaceDataTests.push([node, 0, 0, '""', node, 0, node, 0]);
  replaceDataTests.push([node, 1, 0, '""', node, 1, node, 1]);
  replaceDataTests.push([node, node + ".length", 0, '""', node, node + ".length", node, node + ".length"]);
  replaceDataTests.push([node, 1, 0, '""', node, 1, node, 3]);
  replaceDataTests.push([node, 2, 0, '""', node, 1, node, 3]);
  replaceDataTests.push([node, 3, 0, '""', node, 1, node, 3]);

  replaceDataTests.push([node, 376, 1, '"foo"', node, 0, node, 1]);
  replaceDataTests.push([node, 0, 1, '"foo"', node, 0, node, 0]);
  replaceDataTests.push([node, 1, 1, '"foo"', node, 1, node, 1]);
  replaceDataTests.push([node, node + ".length", 1, '"foo"', node, node + ".length", node, node + ".length"]);
  replaceDataTests.push([node, 1, 1, '"foo"', node, 1, node, 3]);
  replaceDataTests.push([node, 2, 1, '"foo"', node, 1, node, 3]);
  replaceDataTests.push([node, 3, 1, '"foo"', node, 1, node, 3]);

  replaceDataTests.push([node, 376, 1, '""', node, 0, node, 1]);
  replaceDataTests.push([node, 0, 1, '""', node, 0, node, 0]);
  replaceDataTests.push([node, 1, 1, '""', node, 1, node, 1]);
  replaceDataTests.push([node, node + ".length", 1, '""', node, node + ".length", node, node + ".length"]);
  replaceDataTests.push([node, 1, 1, '""', node, 1, node, 3]);
  replaceDataTests.push([node, 2, 1, '""', node, 1, node, 3]);
  replaceDataTests.push([node, 3, 1, '""', node, 1, node, 3]);

  replaceDataTests.push([node, 376, 47, '"foo"', node, 0, node, 1]);
  replaceDataTests.push([node, 0, 47, '"foo"', node, 0, node, 0]);
  replaceDataTests.push([node, 1, 47, '"foo"', node, 1, node, 1]);
  replaceDataTests.push([node, node + ".length", 47, '"foo"', node, node + ".length", node, node + ".length"]);
  replaceDataTests.push([node, 1, 47, '"foo"', node, 1, node, 3]);
  replaceDataTests.push([node, 2, 47, '"foo"', node, 1, node, 3]);
  replaceDataTests.push([node, 3, 47, '"foo"', node, 1, node, 3]);

  replaceDataTests.push([node, 376, 47, '""', node, 0, node, 1]);
  replaceDataTests.push([node, 0, 47, '""', node, 0, node, 0]);
  replaceDataTests.push([node, 1, 47, '""', node, 1, node, 1]);
  replaceDataTests.push([node, node + ".length", 47, '""', node, node + ".length", node, node + ".length"]);
  replaceDataTests.push([node, 1, 47, '""', node, 1, node, 3]);
  replaceDataTests.push([node, 2, 47, '""', node, 1, node, 3]);
  replaceDataTests.push([node, 3, 47, '""', node, 1, node, 3]);
}

replaceDataTests.push(
  ["paras[0].firstChild", 1, 0, '"foo"', "paras[0]", 0, "paras[0]", 0],
  ["paras[0].firstChild", 1, 0, '"foo"', "paras[0]", 0, "paras[0]", 1],
  ["paras[0].firstChild", 1, 0, '"foo"', "paras[0]", 1, "paras[0]", 1],
  ["paras[0].firstChild", 1, 0, '"foo"', "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", 2, 0, '"foo"', "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", 3, 0, '"foo"', "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", 1, 0, '"foo"', "paras[0]", 0, "paras[0].firstChild", 3],
  ["paras[0].firstChild", 2, 0, '"foo"', "paras[0]", 0, "paras[0].firstChild", 3],
  ["paras[0].firstChild", 3, 0, '"foo"', "paras[0]", 0, "paras[0].firstChild", 3],

  ["paras[0].firstChild", 1, 1, '"foo"', "paras[0]", 0, "paras[0]", 0],
  ["paras[0].firstChild", 1, 1, '"foo"', "paras[0]", 0, "paras[0]", 1],
  ["paras[0].firstChild", 1, 1, '"foo"', "paras[0]", 1, "paras[0]", 1],
  ["paras[0].firstChild", 1, 1, '"foo"', "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", 2, 1, '"foo"', "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", 3, 1, '"foo"', "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", 1, 1, '"foo"', "paras[0]", 0, "paras[0].firstChild", 3],
  ["paras[0].firstChild", 2, 1, '"foo"', "paras[0]", 0, "paras[0].firstChild", 3],
  ["paras[0].firstChild", 3, 1, '"foo"', "paras[0]", 0, "paras[0].firstChild", 3],

  ["paras[0].firstChild", 1, 47, '"foo"', "paras[0]", 0, "paras[0]", 0],
  ["paras[0].firstChild", 1, 47, '"foo"', "paras[0]", 0, "paras[0]", 1],
  ["paras[0].firstChild", 1, 47, '"foo"', "paras[0]", 1, "paras[0]", 1],
  ["paras[0].firstChild", 1, 47, '"foo"', "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", 2, 47, '"foo"', "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", 3, 47, '"foo"', "paras[0].firstChild", 1, "paras[0]", 1],
  ["paras[0].firstChild", 1, 47, '"foo"', "paras[0]", 0, "paras[0].firstChild", 3],
  ["paras[0].firstChild", 2, 47, '"foo"', "paras[0]", 0, "paras[0].firstChild", 3],
  ["paras[0].firstChild", 3, 47, '"foo"', "paras[0]", 0, "paras[0].firstChild", 3]
);


// There are lots of ways to set data, so we pass a callback that does the
// actual setting.
function testDataChange(node, attr, op, rval, startContainer, startOffset, endContainer, endOffset) {
  return testReplaceDataAlgorithm(node, 0, node.length, op == "=" ? rval : node[attr] + rval,
    function() {
      if (op == "=") {
        node[attr] = rval;
      } else if (op == "+=") {
        node[attr] += rval;
      } else {
        throw "Unknown op " + op;
      }
    },
    startContainer, startOffset, endContainer, endOffset);
}

var dataChangeTests = [];
var dataChangeTestAttrs = ["data", "textContent", "nodeValue"];
for (var i = 0; i < characterDataNodes.length; i++) {
  var node = characterDataNodes[i];
  var dataChangeTestRanges = [
    [node, 0, node, 0],
    [node, 0, node, 1],
    [node, 1, node, 1],
    [node, 0, node, node + ".length"],
    [node, 1, node, node + ".length"],
    [node, node + ".length", node, node + ".length"],
  ];

  for (var j = 0; j < dataChangeTestRanges.length; j++) {
    for (var k = 0; k < dataChangeTestAttrs.length; k++) {
      dataChangeTests.push([
        node,
        '"' + dataChangeTestAttrs[k] + '"',
        '"="',
        '""',
      ].concat(dataChangeTestRanges[j]));

      dataChangeTests.push([
        node,
        '"' + dataChangeTestAttrs[k] + '"',
        '"="',
        '"foo"',
      ].concat(dataChangeTestRanges[j]));

      dataChangeTests.push([
        node,
        '"' + dataChangeTestAttrs[k] + '"',
        '"="',
        node + "." + dataChangeTestAttrs[k],
      ].concat(dataChangeTestRanges[j]));

      dataChangeTests.push([
        node,
        '"' + dataChangeTestAttrs[k] + '"',
        '"+="',
        '""',
      ].concat(dataChangeTestRanges[j]));

      dataChangeTests.push([
        node,
        '"' + dataChangeTestAttrs[k] + '"',
        '"+="',
        '"foo"',
      ].concat(dataChangeTestRanges[j]));

      dataChangeTests.push([
        node,
        '"' + dataChangeTestAttrs[k] + '"',
        '"+="',
        node + "." + dataChangeTestAttrs[k]
      ].concat(dataChangeTestRanges[j]));
    }
  }
}


// Now we test node insertions and deletions, as opposed to just data changes.
// To avoid loads of repetition, we define modifyForRemove() and
// modifyForInsert().

// If we were to remove removedNode from its parent, what would the boundary
// point [node, offset] become?  Returns [new node, new offset].  Must be
// called BEFORE the node is actually removed, so its parent is not null.  (If
// the parent is null, it will do nothing.)
function modifyForRemove(removedNode, point) {
  var oldParent = removedNode.parentNode;
  var oldIndex = indexOf(removedNode);
  if (!oldParent) {
    return point;
  }

  // "For each boundary point whose node is removed node or a descendant of
  // it, set the boundary point to (old parent, old index)."
  if (point[0] == removedNode || isDescendant(point[0], removedNode)) {
    return [oldParent, oldIndex];
  }

  // "For each boundary point whose node is old parent and whose offset is
  // greater than old index, subtract one from its offset."
  if (point[0] == oldParent && point[1] > oldIndex) {
    return [point[0], point[1] - 1];
  }

  return point;
}

// Update the given boundary point [node, offset] to account for the fact that
// insertedNode was just inserted into its current position.  This must be
// called AFTER insertedNode was already inserted.
function modifyForInsert(insertedNode, point) {
  // "For each boundary point whose node is the new parent of the affected
  // node and whose offset is greater than the new index of the affected
  // node, add one to the boundary point's offset."
  if (point[0] == insertedNode.parentNode && point[1] > indexOf(insertedNode)) {
    return [point[0], point[1] + 1];
  }

  return point;
}


function testInsertBefore(newParent, affectedNode, refNode, startContainer, startOffset, endContainer, endOffset) {
  var expectedStart = [startContainer, startOffset];
  var expectedEnd = [endContainer, endOffset];

  expectedStart = modifyForRemove(affectedNode, expectedStart);
  expectedEnd = modifyForRemove(affectedNode, expectedEnd);

  try {
    newParent.insertBefore(affectedNode, refNode);
  } catch (e) {
    // For our purposes, assume that DOM Core is true -- i.e., ignore
    // mutation events and similar.
    return [startContainer, startOffset, endContainer, endOffset];
  }

  expectedStart = modifyForInsert(affectedNode, expectedStart);
  expectedEnd = modifyForInsert(affectedNode, expectedEnd);

  return expectedStart.concat(expectedEnd);
}

var insertBeforeTests = [
  // Moving a node to its current position
  ["testDiv", "paras[0]", "paras[1]", "paras[0]", 0, "paras[0]", 0],
  ["testDiv", "paras[0]", "paras[1]", "paras[0]", 0, "paras[0]", 1],
  ["testDiv", "paras[0]", "paras[1]", "paras[0]", 1, "paras[0]", 1],
  ["testDiv", "paras[0]", "paras[1]", "testDiv", 0, "testDiv", 2],
  ["testDiv", "paras[0]", "paras[1]", "testDiv", 1, "testDiv", 1],
  ["testDiv", "paras[0]", "paras[1]", "testDiv", 1, "testDiv", 2],
  ["testDiv", "paras[0]", "paras[1]", "testDiv", 2, "testDiv", 2],

  // Stuff that actually moves something.  Note that paras[0] and paras[1]
  // are both children of testDiv.
  ["paras[0]", "paras[1]", "paras[0].firstChild", "paras[0]", 0, "paras[0]", 0],
  ["paras[0]", "paras[1]", "paras[0].firstChild", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "paras[1]", "paras[0].firstChild", "paras[0]", 1, "paras[0]", 1],
  ["paras[0]", "paras[1]", "paras[0].firstChild", "testDiv", 0, "testDiv", 1],
  ["paras[0]", "paras[1]", "paras[0].firstChild", "testDiv", 0, "testDiv", 2],
  ["paras[0]", "paras[1]", "paras[0].firstChild", "testDiv", 1, "testDiv", 1],
  ["paras[0]", "paras[1]", "paras[0].firstChild", "testDiv", 1, "testDiv", 2],
  ["paras[0]", "paras[1]", "null", "paras[0]", 0, "paras[0]", 0],
  ["paras[0]", "paras[1]", "null", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "paras[1]", "null", "paras[0]", 1, "paras[0]", 1],
  ["paras[0]", "paras[1]", "null", "testDiv", 0, "testDiv", 1],
  ["paras[0]", "paras[1]", "null", "testDiv", 0, "testDiv", 2],
  ["paras[0]", "paras[1]", "null", "testDiv", 1, "testDiv", 1],
  ["paras[0]", "paras[1]", "null", "testDiv", 1, "testDiv", 2],
  ["foreignDoc", "detachedComment", "foreignDoc.documentElement", "foreignDoc", 0, "foreignDoc", 0],
  ["foreignDoc", "detachedComment", "foreignDoc.documentElement", "foreignDoc", 0, "foreignDoc", 1],
  ["foreignDoc", "detachedComment", "foreignDoc.documentElement", "foreignDoc", 0, "foreignDoc", 2],
  ["foreignDoc", "detachedComment", "foreignDoc.documentElement", "foreignDoc", 1, "foreignDoc", 1],
  ["foreignDoc", "detachedComment", "foreignDoc.doctype", "foreignDoc", 0, "foreignDoc", 0],
  ["foreignDoc", "detachedComment", "foreignDoc.doctype", "foreignDoc", 0, "foreignDoc", 1],
  ["foreignDoc", "detachedComment", "foreignDoc.doctype", "foreignDoc", 0, "foreignDoc", 2],
  ["foreignDoc", "detachedComment", "foreignDoc.doctype", "foreignDoc", 1, "foreignDoc", 1],
  ["foreignDoc", "detachedComment", "null", "foreignDoc", 0, "foreignDoc", 1],
  ["paras[0]", "xmlTextNode", "paras[0].firstChild", "paras[0]", 0, "paras[0]", 0],
  ["paras[0]", "xmlTextNode", "paras[0].firstChild", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "xmlTextNode", "paras[0].firstChild", "paras[0]", 1, "paras[0]", 1],

  // Stuff that throws exceptions
  ["paras[0]", "paras[0]", "paras[0].firstChild", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "testDiv", "paras[0].firstChild", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "document", "paras[0].firstChild", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "foreignDoc", "paras[0].firstChild", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "document.doctype", "paras[0].firstChild", "paras[0]", 0, "paras[0]", 1],
];


function testReplaceChild(newParent, newChild, oldChild, startContainer, startOffset, endContainer, endOffset) {
  var expectedStart = [startContainer, startOffset];
  var expectedEnd = [endContainer, endOffset];

  expectedStart = modifyForRemove(oldChild, expectedStart);
  expectedEnd = modifyForRemove(oldChild, expectedEnd);

  if (newChild != oldChild) {
    // Don't do this twice, if they're the same!
    expectedStart = modifyForRemove(newChild, expectedStart);
    expectedEnd = modifyForRemove(newChild, expectedEnd);
  }

  try {
    newParent.replaceChild(newChild, oldChild);
  } catch (e) {
    return [startContainer, startOffset, endContainer, endOffset];
  }

  expectedStart = modifyForInsert(newChild, expectedStart);
  expectedEnd = modifyForInsert(newChild, expectedEnd);

  return expectedStart.concat(expectedEnd);
}

var replaceChildTests = [
  // Moving a node to its current position.  Doesn't match most browsers'
  // behavior, but we probably want to keep the spec the same anyway:
  // https://bugzilla.mozilla.org/show_bug.cgi?id=647603
  ["testDiv", "paras[0]", "paras[0]", "paras[0]", 0, "paras[0]", 0],
  ["testDiv", "paras[0]", "paras[0]", "paras[0]", 0, "paras[0]", 1],
  ["testDiv", "paras[0]", "paras[0]", "paras[0]", 1, "paras[0]", 1],
  ["testDiv", "paras[0]", "paras[0]", "testDiv", 0, "testDiv", 2],
  ["testDiv", "paras[0]", "paras[0]", "testDiv", 1, "testDiv", 1],
  ["testDiv", "paras[0]", "paras[0]", "testDiv", 1, "testDiv", 2],
  ["testDiv", "paras[0]", "paras[0]", "testDiv", 2, "testDiv", 2],

  // Stuff that actually moves something.
  ["paras[0]", "paras[1]", "paras[0].firstChild", "paras[0]", 0, "paras[0]", 0],
  ["paras[0]", "paras[1]", "paras[0].firstChild", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "paras[1]", "paras[0].firstChild", "paras[0]", 1, "paras[0]", 1],
  ["paras[0]", "paras[1]", "paras[0].firstChild", "testDiv", 0, "testDiv", 1],
  ["paras[0]", "paras[1]", "paras[0].firstChild", "testDiv", 0, "testDiv", 2],
  ["paras[0]", "paras[1]", "paras[0].firstChild", "testDiv", 1, "testDiv", 1],
  ["paras[0]", "paras[1]", "paras[0].firstChild", "testDiv", 1, "testDiv", 2],
  ["foreignDoc", "detachedComment", "foreignDoc.documentElement", "foreignDoc", 0, "foreignDoc", 0],
  ["foreignDoc", "detachedComment", "foreignDoc.documentElement", "foreignDoc", 0, "foreignDoc", 1],
  ["foreignDoc", "detachedComment", "foreignDoc.documentElement", "foreignDoc", 0, "foreignDoc", 2],
  ["foreignDoc", "detachedComment", "foreignDoc.documentElement", "foreignDoc", 1, "foreignDoc", 1],
  ["foreignDoc", "detachedComment", "foreignDoc.doctype", "foreignDoc", 0, "foreignDoc", 0],
  ["foreignDoc", "detachedComment", "foreignDoc.doctype", "foreignDoc", 0, "foreignDoc", 1],
  ["foreignDoc", "detachedComment", "foreignDoc.doctype", "foreignDoc", 0, "foreignDoc", 2],
  ["foreignDoc", "detachedComment", "foreignDoc.doctype", "foreignDoc", 1, "foreignDoc", 1],
  ["paras[0]", "xmlTextNode", "paras[0].firstChild", "paras[0]", 0, "paras[0]", 0],
  ["paras[0]", "xmlTextNode", "paras[0].firstChild", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "xmlTextNode", "paras[0].firstChild", "paras[0]", 1, "paras[0]", 1],

  // Stuff that throws exceptions
  ["paras[0]", "paras[0]", "paras[0].firstChild", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "testDiv", "paras[0].firstChild", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "document", "paras[0].firstChild", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "foreignDoc", "paras[0].firstChild", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "document.doctype", "paras[0].firstChild", "paras[0]", 0, "paras[0]", 1],
];


function testAppendChild(newParent, affectedNode, startContainer, startOffset, endContainer, endOffset) {
  var expectedStart = [startContainer, startOffset];
  var expectedEnd = [endContainer, endOffset];

  expectedStart = modifyForRemove(affectedNode, expectedStart);
  expectedEnd = modifyForRemove(affectedNode, expectedEnd);

  try {
    newParent.appendChild(affectedNode);
  } catch (e) {
    return [startContainer, startOffset, endContainer, endOffset];
  }

  // These two lines will actually never do anything, if you think about it,
  // but let's leave them in so correctness is more obvious.
  expectedStart = modifyForInsert(affectedNode, expectedStart);
  expectedEnd = modifyForInsert(affectedNode, expectedEnd);

  return expectedStart.concat(expectedEnd);
}

var appendChildTests = [
  // Moving a node to its current position
  ["testDiv", "testDiv.lastChild", "testDiv.lastChild", 0, "testDiv.lastChild", 0],
  ["testDiv", "testDiv.lastChild", "testDiv.lastChild", 0, "testDiv.lastChild", 1],
  ["testDiv", "testDiv.lastChild", "testDiv.lastChild", 1, "testDiv.lastChild", 1],
  ["testDiv", "testDiv.lastChild", "testDiv", "testDiv.childNodes.length - 2", "testDiv", "testDiv.childNodes.length"],
  ["testDiv", "testDiv.lastChild", "testDiv", "testDiv.childNodes.length - 2", "testDiv", "testDiv.childNodes.length - 1"],
  ["testDiv", "testDiv.lastChild", "testDiv", "testDiv.childNodes.length - 1", "testDiv", "testDiv.childNodes.length"],
  ["testDiv", "testDiv.lastChild", "testDiv", "testDiv.childNodes.length - 1", "testDiv", "testDiv.childNodes.length - 1"],
  ["testDiv", "testDiv.lastChild", "testDiv", "testDiv.childNodes.length", "testDiv", "testDiv.childNodes.length"],
  ["detachedDiv", "detachedDiv.lastChild", "detachedDiv.lastChild", 0, "detachedDiv.lastChild", 0],
  ["detachedDiv", "detachedDiv.lastChild", "detachedDiv.lastChild", 0, "detachedDiv.lastChild", 1],
  ["detachedDiv", "detachedDiv.lastChild", "detachedDiv.lastChild", 1, "detachedDiv.lastChild", 1],
  ["detachedDiv", "detachedDiv.lastChild", "detachedDiv", "detachedDiv.childNodes.length - 2", "detachedDiv", "detachedDiv.childNodes.length"],
  ["detachedDiv", "detachedDiv.lastChild", "detachedDiv", "detachedDiv.childNodes.length - 2", "detachedDiv", "detachedDiv.childNodes.length - 1"],
  ["detachedDiv", "detachedDiv.lastChild", "detachedDiv", "detachedDiv.childNodes.length - 1", "detachedDiv", "detachedDiv.childNodes.length"],
  ["detachedDiv", "detachedDiv.lastChild", "detachedDiv", "detachedDiv.childNodes.length - 1", "detachedDiv", "detachedDiv.childNodes.length - 1"],
  ["detachedDiv", "detachedDiv.lastChild", "detachedDiv", "detachedDiv.childNodes.length", "detachedDiv", "detachedDiv.childNodes.length"],

  // Stuff that actually moves something
  ["paras[0]", "paras[1]", "paras[0]", 0, "paras[0]", 0],
  ["paras[0]", "paras[1]", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "paras[1]", "paras[0]", 1, "paras[0]", 1],
  ["paras[0]", "paras[1]", "testDiv", 0, "testDiv", 1],
  ["paras[0]", "paras[1]", "testDiv", 0, "testDiv", 2],
  ["paras[0]", "paras[1]", "testDiv", 1, "testDiv", 1],
  ["paras[0]", "paras[1]", "testDiv", 1, "testDiv", 2],
  ["foreignDoc", "detachedComment", "foreignDoc", "foreignDoc.childNodes.length - 1", "foreignDoc", "foreignDoc.childNodes.length"],
  ["foreignDoc", "detachedComment", "foreignDoc", "foreignDoc.childNodes.length - 1", "foreignDoc", "foreignDoc.childNodes.length - 1"],
  ["foreignDoc", "detachedComment", "foreignDoc", "foreignDoc.childNodes.length", "foreignDoc", "foreignDoc.childNodes.length"],
  ["foreignDoc", "detachedComment", "detachedComment", 0, "detachedComment", 5],
  ["paras[0]", "xmlTextNode", "paras[0]", 0, "paras[0]", 0],
  ["paras[0]", "xmlTextNode", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "xmlTextNode", "paras[0]", 1, "paras[0]", 1],

  // Stuff that throws exceptions
  ["paras[0]", "paras[0]", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "testDiv", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "document", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "foreignDoc", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "document.doctype", "paras[0]", 0, "paras[0]", 1],
];


function testRemoveChild(affectedNode, startContainer, startOffset, endContainer, endOffset) {
  var expectedStart = [startContainer, startOffset];
  var expectedEnd = [endContainer, endOffset];

  expectedStart = modifyForRemove(affectedNode, expectedStart);
  expectedEnd = modifyForRemove(affectedNode, expectedEnd);

  // We don't test cases where the parent is wrong, so this should never
  // throw an exception.
  affectedNode.parentNode.removeChild(affectedNode);

  return expectedStart.concat(expectedEnd);
}

var removeChildTests = [
  ["paras[0]", "paras[0]", 0, "paras[0]", 0],
  ["paras[0]", "paras[0]", 0, "paras[0]", 1],
  ["paras[0]", "paras[0]", 1, "paras[0]", 1],
  ["paras[0]", "testDiv", 0, "testDiv", 0],
  ["paras[0]", "testDiv", 0, "testDiv", 1],
  ["paras[0]", "testDiv", 1, "testDiv", 1],
  ["paras[0]", "testDiv", 0, "testDiv", 2],
  ["paras[0]", "testDiv", 1, "testDiv", 2],
  ["paras[0]", "testDiv", 2, "testDiv", 2],

  ["foreignDoc.documentElement", "foreignDoc", 0, "foreignDoc", "foreignDoc.childNodes.length"],
];
