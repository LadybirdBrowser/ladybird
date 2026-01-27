// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js
// META: timeout=long

'use strict';

idl_test(
  ['fullscreen'],
  ['dom', 'html'],
  idl_array => {
    idl_array.add_objects({
      Document: ['new Document'],
      Element: ['document.createElementNS(null, "test")'],
    });
  }
);
