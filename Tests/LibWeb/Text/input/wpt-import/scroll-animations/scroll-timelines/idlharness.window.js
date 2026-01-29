// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js

'use strict';

idl_test(
  ['scroll-animations'],
  // The css-pseudo dependency shouldn't be necessary, but is:
  // https://github.com/web-platform-tests/wpt/issues/12574
  ['web-animations', 'css-pseudo', 'dom'],
  idl_array => {
    idl_array.add_objects({
      ScrollTimeline: ['new ScrollTimeline()'],
    });
  }
);
