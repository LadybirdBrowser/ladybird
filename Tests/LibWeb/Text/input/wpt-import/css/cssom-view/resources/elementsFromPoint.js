function nodeToString(node) {
  var str = '';
  if (node.nodeType == Node.ELEMENT_NODE) {
    str += node.nodeName;
    if (node.id)
      str += '#' + node.id;
    else if (node.class)
      str += '.' + node.class;
  } else if (node.nodeType == Node.TEXT_NODE) {
    str += '\'' + node.data + '\'';
  } else if (node.nodeType == Node.DOCUMENT_NODE) {
    str += '#document';
  }
  return str;
}

function nodeListToString(nodes) {
  var nodeString = '';

  for (var i = 0; i < nodes.length; i++) {
    var str = nodeToString(nodes[i]);
    if (!str)
      continue;
    nodeString += str;
    if (i + 1 < nodes.length)
      nodeString += ', ';
  }
  return nodeString;
}

function assertElementsFromPoint(doc, x, y, expected) {
  var query = doc + '.elementsFromPoint(' + x + ',' + y + ')';
  var sequence = eval(query);
  assert_equals(nodeListToString(sequence), nodeListToString(expected), query);
}

function checkElementsFromPointFourCorners(doc, element, expectedTopLeft, expectedTopRight, expectedBottomLeft, expectedBottomRight) {
  var rect = eval(doc + '.getElementById(\'' + element + '\')').getBoundingClientRect();
  var topLeft = {x: rect.left + 1, y: rect.top + 1};
  var topRight = {x: rect.right - 1, y: rect.top + 1};
  var bottomLeft = {x: rect.left + 1, y: rect.bottom - 1};
  var bottomRight = {x: rect.right - 1, y: rect.bottom - 1};

  assertElementsFromPoint(doc, topLeft.x, topLeft.y, expectedTopLeft);
  assertElementsFromPoint(doc, topRight.x, topRight.y, expectedTopRight);
  assertElementsFromPoint(doc, bottomLeft.x, bottomLeft.y, expectedBottomLeft);
  assertElementsFromPoint(doc, bottomRight.x, bottomRight.y, expectedBottomRight);
}
