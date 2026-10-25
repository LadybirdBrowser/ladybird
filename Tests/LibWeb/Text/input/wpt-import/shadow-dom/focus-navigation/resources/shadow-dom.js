function removeWhiteSpaceOnlyTextNodes(node) {
  for (var i = 0; i < node.childNodes.length; i++) {
    var child = node.childNodes[i];
    if (child.nodeType === Node.TEXT_NODE &&
      child.nodeValue.trim().length == 0) {
      node.removeChild(child);
      i--;
    } else if (
      child.nodeType === Node.ELEMENT_NODE ||
      child.nodeType === Node.DOCUMENT_FRAGMENT_NODE) {
      removeWhiteSpaceOnlyTextNodes(child);
    }
  }
  if (node.shadowRoot) {
    removeWhiteSpaceOnlyTextNodes(node.shadowRoot);
  }
}

function convertTemplatesToShadowRootsWithin(node) {
  var nodes = node.querySelectorAll('template');
  for (var i = 0; i < nodes.length; ++i) {
    var template = nodes[i];
    var mode = template.getAttribute('data-mode');
    var delegatesFocus = template.hasAttribute('data-delegatesFocus');
    var parent = template.parentNode;
    parent.removeChild(template);
    var shadowRoot;
    if (!mode) {
      shadowRoot = parent.attachShadow({ mode: 'open' });
    } else {
      shadowRoot =
        parent.attachShadow({ 'mode': mode, 'delegatesFocus': delegatesFocus });
    }
    var expose = template.getAttribute('data-expose-as');
    if (expose)
      window[expose] = shadowRoot;
    if (template.id)
      shadowRoot.id = template.id;
    var fragments = document.importNode(template.content, true);
    shadowRoot.appendChild(fragments);

    convertTemplatesToShadowRootsWithin(shadowRoot);
  }
}

function isShadowHost(node) {
  return node && node.nodeType == Node.ELEMENT_NODE && node.shadowRoot;
}

function isIFrameElement(element) {
  return element && element.nodeName == 'IFRAME';
}

// Returns node from shadow/iframe tree "path".
function getNodeInComposedTree(path) {
  var ids = path.split('/');
  var node = document.getElementById(ids[0]);
  for (var i = 1; node != null && i < ids.length; ++i) {
    if (isIFrameElement(node))
      node = node.contentDocument.getElementById(ids[i]);
    else if (isShadowHost(node))
      node = node.shadowRoot.getElementById(ids[i]);
    else
      return null;
  }
  return node;
}

function createTestTree(node) {
  let ids = {};

  function attachShadowFromTemplate(template) {
    let parent = template.parentNode;
    parent.removeChild(template);
    let shadowRoot;
    if (template.getAttribute('data-slot-assignment') === 'manual') {
      shadowRoot =
        parent.attachShadow({
          mode: template.getAttribute('data-mode'),
          slotAssignment: 'manual'
        });
    } else {
      shadowRoot =
        parent.attachShadow({ mode: template.getAttribute('data-mode') });
    }
    let id = template.id;
    if (id) {
      shadowRoot.id = id;
      ids[id] = shadowRoot;
    }
    shadowRoot.appendChild(document.importNode(template.content, true));
    return shadowRoot;
  }

  function walk(root) {
    if (root.id) {
      ids[root.id] = root;
    }
    for (let e of Array.from(root.querySelectorAll('[id]'))) {
      ids[e.id] = e;
    }
    for (let e of Array.from(root.querySelectorAll('template'))) {
      walk(attachShadowFromTemplate(e));
    }
  }

  walk(node.cloneNode(true));
  return ids;
}

function dispatchEventWithLog(nodes, target, event) {
  function labelFor(e) {
    return e.id || e.tagName;
  }

  let log = [];
  let attachedNodes = [];
  for (let label in nodes) {
    let startingNode = nodes[label];
    for (let node = startingNode; node; node = node.parentNode) {
      if (attachedNodes.indexOf(node) >= 0)
        continue;
      let id = node.id;
      if (!id)
        continue;
      attachedNodes.push(node);
      node.addEventListener(event.type, (e) => {
        // Record [currentTarget, target, relatedTarget, composedPath()]
        log.push([
          id, labelFor(e.target),
          e.relatedTarget ? labelFor(e.relatedTarget) : null,
          e.composedPath().map((n) => {
            return labelFor(n);
          })
        ]);
      });
    }
  }
  target.dispatchEvent(event);
  return log;
}

// This function assumes that testharness.js is available.
function assert_event_path_equals(actual, expected) {
  assert_equals(actual.length, expected.length);
  for (let i = 0; i < actual.length; ++i) {
    assert_equals(
      actual[i][0], expected[i][0],
      'currentTarget at ' + i + ' should be same');
    assert_equals(
      actual[i][1], expected[i][1], 'target at ' + i + ' should be same');
    assert_equals(
      actual[i][2], expected[i][2],
      'relatedTarget at ' + i + ' should be same');
    assert_array_equals(
      actual[i][3], expected[i][3],
      'composedPath at ' + i + ' should be same');
  }
}

function assert_background_color(path, color) {
  assert_equals(
    window.getComputedStyle(getNodeInComposedTree(path)).backgroundColor,
    color, 'backgroundColor for ' + path + ' should be ' + color);
}
