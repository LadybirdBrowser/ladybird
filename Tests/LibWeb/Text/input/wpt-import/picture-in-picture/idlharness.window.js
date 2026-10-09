// META: script=/common/media.js
// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js
// META: script=/resources/testdriver.js
// META: script=/resources/testdriver-vendor.js
// META: script=resources/picture-in-picture-helpers.js
// META: timeout=long

'use strict';

// https://wicg.github.io/picture-in-picture/

idl_test(
  ['picture-in-picture'],
  ['html', 'dom'],
  async idl_array => {
    idl_array.add_objects({
      Document: ['document'],
      DocumentOrShadowRoot: ['document'],
      HTMLVideoElement: ['video'],
      PictureInPictureWindow: ['pipw'],
      PictureInPictureEvent: ['new PictureInPictureEvent("type", { pictureInPictureWindow: pipw })'],
    });

    self.video = await loadVideo();
    self.pipw = await requestPictureInPictureWithTrustedClick(video);
  }
);
