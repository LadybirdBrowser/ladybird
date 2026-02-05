// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js

'use strict';

idl_test(
  ['serial'],
  ['html', 'dom'],
  idl_array => {
    idl_array.add_objects({
      Serial: ['navigator.serial'],
      // TODO: SerialPort
      // TODO: SerialPortInfo
    });

    if (self.GLOBAL.isWorker()) {
      idl_array.add_objects({ WorkerNavigator: ['navigator'] });
    } else {
      idl_array.add_objects({ Navigator: ['navigator'] });
    }
  }
);
