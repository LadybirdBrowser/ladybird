(function(window) {
    var SEGMENT_INFO_LIST = [
        {
            url: 'mp4/test.mp4',
            type: 'video/mp4; codecs="mp4a.40.2,avc1.4d400d"',
            duration: 6.549,
            init: { offset: 0, size: 1413 },
            media: [
                { offset: 1413, size: 24034, timev: 0.095000, timea: 0, endtimev: 0.896666, endtimea: 0.882358 },
                { offset: 25447, size: 21757, timev: 0.896666, timea: 0.882358, endtimev: 1.696666, endtimea: 1.671836 },
                { offset: 47204, size: 23591, timev: 1.696666, timea: 1.671836, endtimev: 2.498333, endtimea: 2.461315 },
                { offset: 70795, size: 22614, timev: 2.498333, timea: 2.461315, endtimev: 3.298333, endtimea: 3.297233 },
                { offset: 93409, size: 18353, timev: 3.298333, timea: 3.297233, endtimev: 4.100000, endtimea: 4.086712},
                { offset: 111762, size: 23935, timev: 4.100000, timea: 4.086712, endtimev: 4.900000, endtimea: 4.876190 },
                { offset: 135697, size: 21911, timev: 4.900000, timea: 4.876190, endtimev: 5.701666, endtimea: 5.665668 },
                { offset: 157608, size: 23776, timev: 5.701666, timea: 5.665668, endtimev: 6.501666, endtimea: 6.501587 },
                { offset: 181384, size: 5843, timev: 6.501666, timea: 6.501587, endtimev: 6.501666, endtimea: 6.501678 },
            ]
        },
        {
            url: 'webm/test.webm',
            type: 'video/webm; codecs="vp8, vorbis"',
            duration: 6.552,
            init: { offset: 0, size: 4116 },
            media: [
                {  offset: 4116, size: 26583, timev: 0.112000, timea: 0, endtimev: 0.913000, endtimea: 0.912000 },
                {  offset: 30699, size: 20555, timev: 0.913000, timea: 0.912000, endtimev: 1.714000, endtimea: 1.701000 },
                {  offset: 51254, size: 22668, timev: 1.714000, timea: 1.701000, endtimev: 2.515000, endtimea: 2.514000 },
                {  offset: 73922, size: 21943, timev: 2.515000, timea: 2.514000, endtimev: 3.315000, endtimea: 3.303000 },
                {  offset: 95865, size: 23015, timev: 3.315000, timea: 3.303000, endtimev: 4.116000, endtimea: 4.093000},
                {  offset: 118880, size: 20406, timev: 4.116000, timea: 4.093000, endtimev: 4.917000, endtimea: 4.906000 },
                {  offset: 139286, size: 21537, timev: 4.917000, timea: 4.906000, endtimev: 5.718000, endtimea: 5.695000 },
                {  offset: 160823, size: 24027, timev: 5.718000, timea: 5.695000, endtimev: 6.519000, endtimea: 6.508000 },
                {  offset: 184850, size: 5955, timev: 6.519000, timea: 6.508000, endtimev: 6.577000, endtimea: 6.577000},
            ],
        }
    ];
    EventExpectationsManager = function(test)
    {
        this.test_ = test;
        this.eventTargetList_ = [];
        this.waitCallbacks_ = [];
    };

    EventExpectationsManager.prototype.expectEvent = function(object, eventName, description)
    {
        var eventInfo = { 'target': object, 'type': eventName, 'description': description};
        var expectations = this.getExpectations_(object);
        expectations.push(eventInfo);

        var t = this;
        var waitHandler = this.test_.step_func(this.handleWaitCallback_.bind(this));
        var eventHandler = this.test_.step_func(function(event)
        {
            object.removeEventListener(eventName, eventHandler);
            var expected = expectations[0];
            assert_equals(event.target, expected.target, "Event target match.");
            assert_equals(event.type, expected.type, "Event types match.");
            assert_equals(eventInfo.description, expected.description, "Descriptions match for '" +  event.type + "'.");

            expectations.shift(1);
            if (t.waitCallbacks_.length > 1)
                setTimeout(waitHandler, 0);
            else if (t.waitCallbacks_.length == 1) {
                // Immediately call the callback.
                waitHandler();
            }
        });
        object.addEventListener(eventName, eventHandler);
    };

    EventExpectationsManager.prototype.waitForExpectedEvents = function(callback)
    {
        this.waitCallbacks_.push(callback);
        setTimeout(this.test_.step_func(this.handleWaitCallback_.bind(this)), 0);
    };

    EventExpectationsManager.prototype.expectingEvents = function()
    {
        for (var i = 0; i < this.eventTargetList_.length; ++i) {
            if (this.eventTargetList_[i].expectations.length > 0) {
                return true;
            }
        }
        return false;
    }

    EventExpectationsManager.prototype.handleWaitCallback_ = function()
    {
        if (this.waitCallbacks_.length == 0 || this.expectingEvents())
            return;
        var callback = this.waitCallbacks_.shift(1);
        callback();
    };

    EventExpectationsManager.prototype.getExpectations_ = function(target)
    {
        for (var i = 0; i < this.eventTargetList_.length; ++i) {
            var info = this.eventTargetList_[i];
            if (info.target == target) {
                return info.expectations;
            }
        }
        var expectations = [];
        this.eventTargetList_.push({ 'target': target, 'expectations': expectations });
        return expectations;
    };

    function loadData_(test, url, callback, isBinary)
    {
        var request = new XMLHttpRequest();
        request.open("GET", url, true);
        if (isBinary) {
            request.responseType = 'arraybuffer';
        }
        request.onload = test.step_func(function(event)
        {
            if (request.status != 200) {
                assert_unreached("Unexpected status code : " + request.status);
                return;
            }
            var response = request.response;
            if (isBinary) {
                response = new Uint8Array(response);
            }
            callback(response);
        });
        request.onerror = test.step_func(function(event)
        {
            assert_unreached("Unexpected error");
        });
        request.send();
    }

    function openMediaSource_(test, mediaTag, callback)
    {
        var mediaSource = new MediaSource();
        var mediaSourceURL = URL.createObjectURL(mediaSource);

        var eventHandler = test.step_func(onSourceOpen);
        function onSourceOpen(event)
        {
            mediaSource.removeEventListener('sourceopen', eventHandler);
            URL.revokeObjectURL(mediaSourceURL);
            callback(mediaSource);
        }

        mediaSource.addEventListener('sourceopen', eventHandler);
        mediaTag.src = mediaSourceURL;
    }

    var MediaSourceUtil = {};

    MediaSourceUtil.loadTextData = function(test, url, callback)
    {
        loadData_(test, url, callback, false);
    };

    MediaSourceUtil.loadBinaryData = function(test, url, callback)
    {
        loadData_(test, url, callback, true);
    };

    /**
     * Return a promise that resolves to an object having the properties
     * specified in the manifest plus a 'data' property with the media data.
     */
    MediaSourceUtil.fetchResourceOfManifest = function(test, manifestFilename)
    {
        var baseURL = '';
        var manifestURL = baseURL + manifestFilename;
        return new Promise(resolve => {
            MediaSourceUtil.loadTextData(test, manifestURL, function(manifestText)
            {
                 var manifest = JSON.parse(manifestText);

                 assert_true(MediaSource.isTypeSupported(manifest.type), manifest.type + " is supported.");

                 var mediaURL = manifest.url;
                 MediaSourceUtil.loadBinaryData(test, mediaURL, function(mediaData)
                 {
                     manifest.data = mediaData;
                     resolve(manifest);
                 });
            });
        });
    };

    MediaSourceUtil.fetchManifestAndData = async function(test, manifestFilename, callback)
    {
        const resource = await MediaSourceUtil.fetchResourceOfManifest(test, manifestFilename);
        test.step(() => callback(resource.type, resource.data));
    };

    MediaSourceUtil.extractSegmentData = function(mediaData, info)
    {
        var start = info.offset;
        var end = start + info.size;
        return mediaData.subarray(start, end);
    }

    MediaSourceUtil.WriteBigEndianInteger32ToUint8Array = function(integer32, array)
    {
        array[0] = integer32 >> 24;
        array[1] = integer32 >> 16;
        array[2] = integer32 >> 8;
        array[3] = integer32;
    }

    MediaSourceUtil.getMediaDataForPlaybackTime = function(mediaData, segmentInfo, playbackTimeToAdd)
    {
        assert_less_than_equal(playbackTimeToAdd, segmentInfo.duration);
        var mediaInfo = segmentInfo.media;
        var start = mediaInfo[0].offset;
        var numBytes = 0;
        var segmentIndex = 0;
        while (segmentIndex < mediaInfo.length
               && Math.min(mediaInfo[segmentIndex].timev, mediaInfo[segmentIndex].timea) <= playbackTimeToAdd)
        {
          numBytes += mediaInfo[segmentIndex].size;
          ++segmentIndex;
        }
        return mediaData.subarray(start, numBytes + start);
    }

    function getFirstSupportedType(typeList)
    {
        for (var i = 0; i < typeList.length; ++i) {
            if (window.MediaSource && MediaSource.isTypeSupported(typeList[i]))
                return typeList[i];
        }
        return "";
    }

    function getSegmentInfo()
    {
        for (var i = 0; i < SEGMENT_INFO_LIST.length; ++i) {
            var segmentInfo = SEGMENT_INFO_LIST[i];
            if (window.MediaSource && MediaSource.isTypeSupported(segmentInfo.type)) {
                return segmentInfo;
            }
        }
        return null;
    }

    // To support mediasource-changetype tests, do not use any types that
    // indicate automatic timestamp generation in this audioOnlyTypes list.
    var audioOnlyTypes = ['audio/mp4;codecs="mp4a.40.2"', 'audio/webm;codecs="vorbis"'];

    var videoOnlyTypes = ['video/mp4;codecs="avc1.4D4001"', 'video/webm;codecs="vp8"'];
    var audioVideoTypes = ['video/mp4;codecs="avc1.4D4001,mp4a.40.2"', 'video/webm;codecs="vp8,vorbis"'];
    MediaSourceUtil.AUDIO_ONLY_TYPE = getFirstSupportedType(audioOnlyTypes);
    MediaSourceUtil.VIDEO_ONLY_TYPE = getFirstSupportedType(videoOnlyTypes);
    MediaSourceUtil.AUDIO_VIDEO_TYPE = getFirstSupportedType(audioVideoTypes);
    MediaSourceUtil.SEGMENT_INFO = getSegmentInfo();

    MediaSourceUtil.getSubType = function(mimetype) {
        var slashIndex = mimetype.indexOf("/");
        var semicolonIndex = mimetype.indexOf(";");
        if (slashIndex <= 0) {
            assert_unreached("Invalid mimetype '" + mimetype + "'");
            return;
        }

        var start = slashIndex + 1;
        if (semicolonIndex >= 0) {
            if (semicolonIndex <= start) {
                assert_unreached("Invalid mimetype '" + mimetype + "'");
                return;
            }

            return mimetype.substr(start, semicolonIndex - start)
        }

        return mimetype.substr(start);
    };

    MediaSourceUtil.append = function(test, sourceBuffer, data, callback)
    {
        function onUpdate() {
            sourceBuffer.removeEventListener("update", onUpdate);
            callback();
        }
        sourceBuffer.addEventListener("update", onUpdate);

        sourceBuffer.addEventListener('error', test.unreached_func("Unexpected event 'error'"));

        sourceBuffer.appendBuffer(data);
    };

    MediaSourceUtil.appendUntilEventFires = function(test, mediaElement, eventName, sourceBuffer, mediaData, segmentInfo, startingIndex)
    {
        var eventFired = false;
        function onEvent() {
            mediaElement.removeEventListener(eventName, onEvent);
            eventFired = true;
        }
        mediaElement.addEventListener(eventName, onEvent);

        var i = startingIndex;
        var onAppendDone = function() {
            if (eventFired || (i >= (segmentInfo.media.length - 1)))
                return;

            i++;
            if (i < segmentInfo.media.length)
            {
                MediaSourceUtil.append(test, sourceBuffer, MediaSourceUtil.extractSegmentData(mediaData, segmentInfo.media[i]), onAppendDone);
            }
        };
        MediaSourceUtil.append(test, sourceBuffer, MediaSourceUtil.extractSegmentData(mediaData, segmentInfo.media[i]), onAppendDone);

    };

    function addExtraTestMethods(test)
    {
        test.eventExpectations_ = new EventExpectationsManager(test);
        test.expectEvent = function(object, eventName, description)
        {
            test.eventExpectations_.expectEvent(object, eventName, description);
        };

        test.waitForExpectedEvents = function(callback)
        {
            test.eventExpectations_.waitForExpectedEvents(callback);
        };

        test.waitForCurrentTimeChange = function(mediaElement, callback)
        {
            var initialTime = mediaElement.currentTime;

            var onTimeUpdate = test.step_func(function()
            {
                if (mediaElement.currentTime != initialTime) {
                    mediaElement.removeEventListener('timeupdate', onTimeUpdate);
                    callback();
                }
            });

            mediaElement.addEventListener('timeupdate', onTimeUpdate);
        }

        var oldTestDone = test.done.bind(test);
        test.done = function()
        {
            if (test.status == test.PASS) {
                test.step(function() {
                    assert_false(test.eventExpectations_.expectingEvents(), "No pending event expectations.");
                });
            }
            oldTestDone();
        };
    };

    window['MediaSourceUtil'] = MediaSourceUtil;
    window['media_test'] = function(testFunction, description, options)
    {
        options = options || {};
        return async_test(function(test)
        {
            addExtraTestMethods(test);
            testFunction(test);
        }, description, options);
    };
    window['mediasource_test'] = function(testFunction, description, options)
    {
        return media_test(function(test)
        {
            var mediaTag = document.createElement("video");
            if (!document.body) {
                document.body = document.createElement("body");
            }
            document.body.appendChild(mediaTag);

            test.removeMediaElement_ = true;
            test.add_cleanup(function()
            {
                if (test.removeMediaElement_) {
                    document.body.removeChild(mediaTag);
                    test.removeMediaElement_ = false;
                }
            });

            openMediaSource_(test, mediaTag, function(mediaSource)
            {
                testFunction(test, mediaTag, mediaSource);
            });
        }, description, options);
    };

    window['mediasource_testafterdataloaded'] = function(testFunction, description, options)
    {
        mediasource_test(function(test, mediaElement, mediaSource)
        {
            var segmentInfo = MediaSourceUtil.SEGMENT_INFO;

            if (!segmentInfo) {
                assert_unreached("No segment info compatible with this MediaSource implementation.");
                return;
            }

            mediaElement.addEventListener('error', test.unreached_func("Unexpected event 'error'"));

            var sourceBuffer = mediaSource.addSourceBuffer(segmentInfo.type);
            MediaSourceUtil.loadBinaryData(test, segmentInfo.url, function(mediaData)
            {
                testFunction(test, mediaElement, mediaSource, segmentInfo, sourceBuffer, mediaData);
            });
        }, description, options);
    }

    function timeRangesToString(ranges)
    {
        var s = "{";
        for (var i = 0; i < ranges.length; ++i) {
            s += " [" + ranges.start(i).toFixed(3) + ", " + ranges.end(i).toFixed(3) + ")";
        }
        return s + " }";
    }

    window['assertBufferedEquals'] = function(obj, expected, description)
    {
        var actual = timeRangesToString(obj.buffered);
        assert_equals(actual, expected, description);
    };

    window['assertSeekableEquals'] = function(obj, expected, description)
    {
        var actual = timeRangesToString(obj.seekable);
        assert_equals(actual, expected, description);
    };

})(window);
