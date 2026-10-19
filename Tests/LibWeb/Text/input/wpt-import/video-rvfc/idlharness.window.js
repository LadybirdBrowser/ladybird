// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js
// META: timeout=long

'use strict';

idl_test(
  ['video-rvfc'],
  ['html', 'dom'],
  idl_array => {
    idl_array.add_objects({
      HTMLVideoElement: ['video'],
    });
    self.video = document.createElement('video');
  }
);

