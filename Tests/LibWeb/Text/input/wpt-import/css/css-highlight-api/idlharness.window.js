// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js

// https://drafts.csswg.org/css-highlight-api-1/

'use strict';

idl_test(
  ['css-highlight-api'],
  ['cssom'],
  idl_array => {
    idl_array.add_objects({
      Highlight: ['new Highlight(new Range())'],
      HighlightRegistry: ['CSS.highlights'],
    });
  }
);
