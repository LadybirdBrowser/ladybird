// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js
// META: timeout=long

// https://w3c.github.io/gamepad/

'use strict';

idl_test(
  ['gamepad'],
  ['html', 'dom'],
  idl_array => {
    idl_array.add_objects({
      GamepadEvent: ['new GamepadEvent("gamepad")'],
      Navigator: ['navigator']
    });
  }
);
