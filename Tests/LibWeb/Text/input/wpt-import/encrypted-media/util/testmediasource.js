function testmediasource(config) {

    return new Promise(function(resolve, reject) {
        // Fetch the media resources
        var fetches = [config.audioPath, config.videoPath].map(function(path) {
            return fetch(path).then(function(response) {
                if (!response.ok) throw new Error('Resource fetch failed');
                return response.arrayBuffer();
            });
        });

        Promise.all(fetches).then(function(resources) {
            config.audioMedia = resources[0];
            config.videoMedia = resources[1];
            if (Object.hasOwn(config, 'audioBlankBytes')) {
                audioMedia = new Uint8Array(config.audioMedia);
                for (const pos of config.audioBlankBytes) {
                    audioMedia[pos] = ' '.charCodeAt();
                }
            }
            if (Object.hasOwn(config, 'videoBlankBytes')) {
                videoMedia = new Uint8Array(config.videoMedia);
                for (const pos of config.videoBlankBytes) {
                    videoMedia[pos] = ' '.charCodeAt();
                }
            }

            // Create media source
            var source = new MediaSource();
            source.done = new Promise(function(resolvesource,rejectsource){

                // Create and fill source buffers when the media source is opened
                source.addEventListener('sourceopen', onSourceOpen);
                resolve(source);

                function onSourceOpen(event) {
                    var audioSourceBuffer = source.addSourceBuffer(config.audioType),
                        videoSourceBuffer = source.addSourceBuffer(config.videoType);

                    audioSourceBuffer.addEventListener('updateend',onUpdateEnd);
                    videoSourceBuffer.addEventListener('updateend',onUpdateEnd);

                    audioSourceBuffer.appendBuffer(config.audioMedia);
                    videoSourceBuffer.appendBuffer(config.videoMedia);

                    function onUpdateEnd(event){
                        event.target.removeEventListener('updateend', onUpdateEnd);
                        if (!audioSourceBuffer.updating && !videoSourceBuffer.updating) {
                            if (source.readyState !== 'open') {
                                rejectsource(new Error("Media source error"));
                            } else {
                                source.endOfStream();
                                resolvesource();
                            }
                        }
                    }
                }
            });
        });
    });
}
