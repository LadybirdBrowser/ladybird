// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js

'use strict';

idl_test(
  ['input-events'],
  ['uievents', 'dom'],
  idl_array => {
    idl_array.add_objects({
      InputEvent: ['new InputEvent("foo")'],
    });
  }
);
