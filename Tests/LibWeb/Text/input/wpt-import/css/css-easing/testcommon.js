'use strict';

// Creates a <div> element, appends it to the document body and
// removes the created element during test cleanup.
function createDiv(test, doc) {
  return createElement(test, 'div', doc);
}

// Creates an element with the given |tagName|, appends it to the document body
// and removes the created element during test cleanup.
// If |tagName| is null or undefined, creates a <div> element.
function createElement(test, tagName, doc) {
  if (!doc) {
    doc = document;
  }
  var element = doc.createElement(tagName || 'div');
  doc.body.appendChild(element);
  test.add_cleanup(function() {
    element.remove();
  });
  return element;
}

// Convert px unit value to a Number
function pxToNum(str) {
  return Number(String(str).match(/^(-?[\d.]+)px$/)[1]);
}

// Cubic bezier with control points (0, 0), (x1, y1), (x2, y2), and (1, 1).
function cubicBezier(x1, y1, x2, y2) {
  function xForT(t) {
    var omt = 1-t;
    return 3 * omt * omt * t * x1 + 3 * omt * t * t * x2 + t * t * t;
  }

  function yForT(t) {
    var omt = 1-t;
    return 3 * omt * omt * t * y1 + 3 * omt * t * t * y2 + t * t * t;
  }

  function tForX(x) {
    // Binary subdivision.
    var mint = 0, maxt = 1;
    for (var i = 0; i < 30; ++i) {
      var guesst = (mint + maxt) / 2;
      var guessx = xForT(guesst);
      if (x < guessx) {
        maxt = guesst;
      } else {
        mint = guesst;
      }
    }
    return (mint + maxt) / 2;
  }

  return function bezierClosure(x) {
    if (x == 0) {
      return 0;
    }
    if (x == 1) {
      return 1;
    }
    return yForT(tForX(x));
  }
}
