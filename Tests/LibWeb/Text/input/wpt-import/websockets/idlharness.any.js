// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js

// https://websockets.spec.whatwg.org/

"use strict";

idl_test(
  ['websockets'],
  ['html', 'dom'],
  idl_array => {
    idl_array.add_objects({
      WebSocket: ['new WebSocket("ws://invalid")'],
      CloseEvent: ['new CloseEvent("close")'],
    });
  }
);
