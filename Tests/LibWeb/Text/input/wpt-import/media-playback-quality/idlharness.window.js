// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js

// https://w3c.github.io/media-playback-quality/

'use strict';

idl_test(
  ['media-playback-quality'],
  ['html', 'dom'],
  idl_array => {
    idl_array.add_objects({
      HTMLVideoElement: ['video'],
      VideoPlaybackQuality: ['videoPlaybackQuality']
    });

    self.video = document.createElement('video');
    self.videoPlaybackQuality = video.getVideoPlaybackQuality();
  }
);
