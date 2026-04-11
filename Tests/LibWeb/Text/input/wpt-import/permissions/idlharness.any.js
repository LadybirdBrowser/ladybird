// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js

// https://w3c.github.io/permissions/#idl-index

"use strict";

idl_test(
  ['permissions'],
  ['html', 'dom'],
  async idl_array => {
    try {
      self.permissionStatus = await navigator.permissions.query({ name: "notifications" });
    } catch (e) {}

    if (self.GLOBAL.isWorker()) {
      idl_array.add_objects({ WorkerNavigator: ['navigator'] });
    } else {
      idl_array.add_objects({ Navigator: ['navigator'] });
    }

    idl_array.add_objects({
      Permissions: ['navigator.permissions'],
      PermissionStatus: ['permissionStatus']
    });
  }
);
