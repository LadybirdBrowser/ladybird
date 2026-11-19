content = addMemberListToObject( {

    'mp4-clear' : {     initDataType:   'cenc',
                        audio : {   type:   'audio/mp4;codecs="mp4a.40.2"',
                                    path:   '/encrypted-media/content/audio_aac-lc_128k_dashinit.mp4' },
                        video : {   type:   'video/mp4;codecs="avc1.4d401e"',
                                    path:   '/encrypted-media/content/video_512x288_h264-360k_clear_dashinit.mp4' }
                    },

    'mp4-basic' : {     assetId:        'mp4-basic',
                        initDataType:   'cenc',
                        audio : {   type:   'audio/mp4;codecs="mp4a.40.2"',
                                    path:   '/encrypted-media/content/audio_aac-lc_128k_dashinit.mp4' },
                        video : {   type:   'video/mp4;codecs="avc1.4d401e"',
                                    path:   '/encrypted-media/content/video_512x288_h264-360k_enc_dashinit.mp4' },
                        keys :  [ { kid: [ 0xad, 0x13, 0xf9, 0xea, 0x2b, 0xe6, 0x98, 0xb8, 0x75, 0xf5, 0x04, 0xa8, 0xe3, 0xcc, 0xea, 0x64 ],
                                    key: [ 0xbe, 0x7d, 0xf8, 0xa3, 0x66, 0x7a, 0x6a, 0x8f, 0xd5, 0x64, 0xd0, 0xed, 0x81, 0x33, 0x9a, 0x95 ],
                                    initDataType: 'cenc',
                                    initData: 'AAAAcXBzc2gAAAAA7e+LqXnWSs6jyCfc1R0h7QAAAFEIARIQrRP56ivmmLh19QSo48zqZBoIY2FzdGxhYnMiKGV5SmhjM05sZEVsa0lqb2laVzFsTFhSbGMzUXRjMmx1WjJ4bEluMD0yB2RlZmF1bHQAAAMacHNzaAAAAACaBPB5mEBChquS5lvgiF+VAAAC+voCAAABAAEA8AI8AFcAUgBNAEgARQBBAEQARQBSACAAeABtAGwAbgBzAD0AIgBoAHQAdABwADoALwAvAHMAYwBoAGUAbQBhAHMALgBtAGkAYwByAG8AcwBvAGYAdAAuAGMAbwBtAC8ARABSAE0ALwAyADAAMAA3AC8AMAAzAC8AUABsAGEAeQBSAGUAYQBkAHkASABlAGEAZABlAHIAIgAgAHYAZQByAHMAaQBvAG4APQAiADQALgAwAC4AMAAuADAAIgA+ADwARABBAFQAQQA+ADwAUABSAE8AVABFAEMAVABJAE4ARgBPAD4APABLAEUAWQBMAEUATgA+ADEANgA8AC8ASwBFAFkATABFAE4APgA8AEEATABHAEkARAA+AEEARQBTAEMAVABSADwALwBBAEwARwBJAEQAPgA8AC8AUABSAE8AVABFAEMAVABJAE4ARgBPAD4APABLAEkARAA+ADYAdgBrAFQAcgBlAFkAcgB1AEoAaAAxADkAUQBTAG8ANAA4AHoAcQBaAEEAPQA9ADwALwBLAEkARAA+ADwAQwBIAEUAQwBLAFMAVQBNAD4AagBZAEYATgBmADAAeQBmADQAaQBzAD0APAAvAEMASABFAEMASwBTAFUATQA+ADwATABBAF8AVQBSAEwAPgBoAHQAdABwADoALwAvAHAAbABhAHkAcgBlAGEAZAB5AC4AZABpAHIAZQBjAHQAdABhAHAAcwAuAG4AZQB0AC8AcAByAC8AcwB2AGMALwByAGkAZwBoAHQAcwBtAGEAbgBhAGcAZQByAC4AYQBzAG0AeAA/AFAAbABhAHkAUgBpAGcAaAB0AD0AMQAmAGEAbQBwADsAVQBzAGUAUwBpAG0AcABsAGUATgBvAG4AUABlAHIAcwBpAHMAdABlAG4AdABMAGkAYwBlAG4AcwBlAD0AMQA8AC8ATABBAF8AVQBSAEwAPgA8AC8ARABBAFQAQQA+ADwALwBXAFIATQBIAEUAQQBEAEUAUgA+AA==' } ]
                    },

    'mp4-clear-encrypted' : {
                        assetId:        'mp4-basic',
                        initDataType:   'cenc',
                        audio : {   type:   'audio/mp4;codecs="mp4a.40.2"',
                                    path:   '/encrypted-media/content/audio_aac-lc_128k_dashinit.mp4' },
                        video : {   type:   'video/mp4;codecs="avc1.4d401e"',
                                    path:   '/encrypted-media/content/video_512x288_h264-360k_clear_enc_dashinit.mp4' },
                        keys :  [ { kid: [ 0xad, 0x13, 0xf9, 0xea, 0x2b, 0xe6, 0x98, 0xb8, 0x75, 0xf5, 0x04, 0xa8, 0xe3, 0xcc, 0xea, 0x64 ],
                                    key: [ 0xbe, 0x7d, 0xf8, 0xa3, 0x66, 0x7a, 0x6a, 0x8f, 0xd5, 0x64, 0xd0, 0xed, 0x81, 0x33, 0x9a, 0x95 ],
                                    initDataType: 'cenc',
                                    initData: 'AAAAcXBzc2gAAAAA7e+LqXnWSs6jyCfc1R0h7QAAAFEIARIQrRP56ivmmLh19QSo48zqZBoIY2FzdGxhYnMiKGV5SmhjM05sZEVsa0lqb2laVzFsTFhSbGMzUXRjMmx1WjJ4bEluMD0yB2RlZmF1bHQAAAMacHNzaAAAAACaBPB5mEBChquS5lvgiF+VAAAC+voCAAABAAEA8AI8AFcAUgBNAEgARQBBAEQARQBSACAAeABtAGwAbgBzAD0AIgBoAHQAdABwADoALwAvAHMAYwBoAGUAbQBhAHMALgBtAGkAYwByAG8AcwBvAGYAdAAuAGMAbwBtAC8ARABSAE0ALwAyADAAMAA3AC8AMAAzAC8AUABsAGEAeQBSAGUAYQBkAHkASABlAGEAZABlAHIAIgAgAHYAZQByAHMAaQBvAG4APQAiADQALgAwAC4AMAAuADAAIgA+ADwARABBAFQAQQA+ADwAUABSAE8AVABFAEMAVABJAE4ARgBPAD4APABLAEUAWQBMAEUATgA+ADEANgA8AC8ASwBFAFkATABFAE4APgA8AEEATABHAEkARAA+AEEARQBTAEMAVABSADwALwBBAEwARwBJAEQAPgA8AC8AUABSAE8AVABFAEMAVABJAE4ARgBPAD4APABLAEkARAA+ADYAdgBrAFQAcgBlAFkAcgB1AEoAaAAxADkAUQBTAG8ANAA4AHoAcQBaAEEAPQA9ADwALwBLAEkARAA+ADwAQwBIAEUAQwBLAFMAVQBNAD4AagBZAEYATgBmADAAeQBmADQAaQBzAD0APAAvAEMASABFAEMASwBTAFUATQA+ADwATABBAF8AVQBSAEwAPgBoAHQAdABwADoALwAvAHAAbABhAHkAcgBlAGEAZAB5AC4AZABpAHIAZQBjAHQAdABhAHAAcwAuAG4AZQB0AC8AcAByAC8AcwB2AGMALwByAGkAZwBoAHQAcwBtAGEAbgBhAGcAZQByAC4AYQBzAG0AeAA/AFAAbABhAHkAUgBpAGcAaAB0AD0AMQAmAGEAbQBwADsAVQBzAGUAUwBpAG0AcABsAGUATgBvAG4AUABlAHIAcwBpAHMAdABlAG4AdABMAGkAYwBlAG4AcwBlAD0AMQA8AC8ATABBAF8AVQBSAEwAPgA8AC8ARABBAFQAQQA+ADwALwBXAFIATQBIAEUAQQBEAEUAUgA+AA==' } ]
                    },

    'mp4-encrypted-clear' : {
                        assetId:        'mp4-basic',
                        initDataType:   'cenc',
                        audio : {   type:   'audio/mp4;codecs="mp4a.40.2"',
                                    path:   '/encrypted-media/content/audio_aac-lc_128k_dashinit.mp4' },
                        video : {   type:   'video/mp4;codecs="avc1.4d401e"',
                                    path:   '/encrypted-media/content/video_512x288_h264-360k_enc_clear_dashinit.mp4' },
                        keys :  [ { kid: [ 0xad, 0x13, 0xf9, 0xea, 0x2b, 0xe6, 0x98, 0xb8, 0x75, 0xf5, 0x04, 0xa8, 0xe3, 0xcc, 0xea, 0x64 ],
                                    key: [ 0xbe, 0x7d, 0xf8, 0xa3, 0x66, 0x7a, 0x6a, 0x8f, 0xd5, 0x64, 0xd0, 0xed, 0x81, 0x33, 0x9a, 0x95 ],
                                    initDataType: 'cenc',
                                    initData: 'AAAAcXBzc2gAAAAA7e+LqXnWSs6jyCfc1R0h7QAAAFEIARIQrRP56ivmmLh19QSo48zqZBoIY2FzdGxhYnMiKGV5SmhjM05sZEVsa0lqb2laVzFsTFhSbGMzUXRjMmx1WjJ4bEluMD0yB2RlZmF1bHQAAAMacHNzaAAAAACaBPB5mEBChquS5lvgiF+VAAAC+voCAAABAAEA8AI8AFcAUgBNAEgARQBBAEQARQBSACAAeABtAGwAbgBzAD0AIgBoAHQAdABwADoALwAvAHMAYwBoAGUAbQBhAHMALgBtAGkAYwByAG8AcwBvAGYAdAAuAGMAbwBtAC8ARABSAE0ALwAyADAAMAA3AC8AMAAzAC8AUABsAGEAeQBSAGUAYQBkAHkASABlAGEAZABlAHIAIgAgAHYAZQByAHMAaQBvAG4APQAiADQALgAwAC4AMAAuADAAIgA+ADwARABBAFQAQQA+ADwAUABSAE8AVABFAEMAVABJAE4ARgBPAD4APABLAEUAWQBMAEUATgA+ADEANgA8AC8ASwBFAFkATABFAE4APgA8AEEATABHAEkARAA+AEEARQBTAEMAVABSADwALwBBAEwARwBJAEQAPgA8AC8AUABSAE8AVABFAEMAVABJAE4ARgBPAD4APABLAEkARAA+ADYAdgBrAFQAcgBlAFkAcgB1AEoAaAAxADkAUQBTAG8ANAA4AHoAcQBaAEEAPQA9ADwALwBLAEkARAA+ADwAQwBIAEUAQwBLAFMAVQBNAD4AagBZAEYATgBmADAAeQBmADQAaQBzAD0APAAvAEMASABFAEMASwBTAFUATQA+ADwATABBAF8AVQBSAEwAPgBoAHQAdABwADoALwAvAHAAbABhAHkAcgBlAGEAZAB5AC4AZABpAHIAZQBjAHQAdABhAHAAcwAuAG4AZQB0AC8AcAByAC8AcwB2AGMALwByAGkAZwBoAHQAcwBtAGEAbgBhAGcAZQByAC4AYQBzAG0AeAA/AFAAbABhAHkAUgBpAGcAaAB0AD0AMQAmAGEAbQBwADsAVQBzAGUAUwBpAG0AcABsAGUATgBvAG4AUABlAHIAcwBpAHMAdABlAG4AdABMAGkAYwBlAG4AcwBlAD0AMQA8AC8ATABBAF8AVQBSAEwAPgA8AC8ARABBAFQAQQA+ADwALwBXAFIATQBIAEUAQQBEAEUAUgA+AA==' } ]
                    },


    'mp4-av-multikey' : {
                        assetId:        'mp4-basic',
                        initDataType:   'cenc',
                        associatedInitData: true,       // indicates that initData for one key causes other keys to be returned as well
                        audio:  {   type:   'audio/mp4;codecs="mp4a.40.2"',
                                    path:   '/encrypted-media/content/audio_aac-lc_128k_enc_dashinit.mp4',
                                    sinfStart:  0x02aa,
                                    saizStarts: [0x081b, 0x9065, 0x1150a],
                                    saioStarts: [0x0802, 0x906e, 0x11523],
                                    sencStarts: [0x9c3, 0x9226] },
                        video : {   type:   'video/mp4;codecs="avc1.4d401e"',
                                    path:   '/encrypted-media/content/video_512x288_h264-360k_enc_dashinit.mp4',
                                    saizStarts: [0x840, 0x18031, 0x2ebad],
                                    saioStarts: [0x889, 0x1804a, 0x2ebc6],
                                    sencStarts: [0x2ec62] },
                        keys :  [ { kid: [ 0xad, 0x13, 0xf9, 0xea, 0x2b, 0xe6, 0x98, 0xb8, 0x75, 0xf5, 0x04, 0xa8, 0xe3, 0xcc, 0xea, 0x64 ],
                                    key: [ 0xbe, 0x7d, 0xf8, 0xa3, 0x66, 0x7a, 0x6a, 0x8f, 0xd5, 0x64, 0xd0, 0xed, 0x81, 0x33, 0x9a, 0x95 ],
                                    initDataType: 'cenc',
                                    initData: 'AAAAcXBzc2gAAAAA7e+LqXnWSs6jyCfc1R0h7QAAAFEIARIQrRP56ivmmLh19QSo48zqZBoIY2FzdGxhYnMiKGV5SmhjM05sZEVsa0lqb2laVzFsTFhSbGMzUXRjMmx1WjJ4bEluMD0yB2RlZmF1bHQAAAMacHNzaAAAAACaBPB5mEBChquS5lvgiF+VAAAC+voCAAABAAEA8AI8AFcAUgBNAEgARQBBAEQARQBSACAAeABtAGwAbgBzAD0AIgBoAHQAdABwADoALwAvAHMAYwBoAGUAbQBhAHMALgBtAGkAYwByAG8AcwBvAGYAdAAuAGMAbwBtAC8ARABSAE0ALwAyADAAMAA3AC8AMAAzAC8AUABsAGEAeQBSAGUAYQBkAHkASABlAGEAZABlAHIAIgAgAHYAZQByAHMAaQBvAG4APQAiADQALgAwAC4AMAAuADAAIgA+ADwARABBAFQAQQA+ADwAUABSAE8AVABFAEMAVABJAE4ARgBPAD4APABLAEUAWQBMAEUATgA+ADEANgA8AC8ASwBFAFkATABFAE4APgA8AEEATABHAEkARAA+AEEARQBTAEMAVABSADwALwBBAEwARwBJAEQAPgA8AC8AUABSAE8AVABFAEMAVABJAE4ARgBPAD4APABLAEkARAA+ADYAdgBrAFQAcgBlAFkAcgB1AEoAaAAxADkAUQBTAG8ANAA4AHoAcQBaAEEAPQA9ADwALwBLAEkARAA+ADwAQwBIAEUAQwBLAFMAVQBNAD4AagBZAEYATgBmADAAeQBmADQAaQBzAD0APAAvAEMASABFAEMASwBTAFUATQA+ADwATABBAF8AVQBSAEwAPgBoAHQAdABwADoALwAvAHAAbABhAHkAcgBlAGEAZAB5AC4AZABpAHIAZQBjAHQAdABhAHAAcwAuAG4AZQB0AC8AcAByAC8AcwB2AGMALwByAGkAZwBoAHQAcwBtAGEAbgBhAGcAZQByAC4AYQBzAG0AeAA/AFAAbABhAHkAUgBpAGcAaAB0AD0AMQAmAGEAbQBwADsAVQBzAGUAUwBpAG0AcABsAGUATgBvAG4AUABlAHIAcwBpAHMAdABlAG4AdABMAGkAYwBlAG4AcwBlAD0AMQA8AC8ATABBAF8AVQBSAEwAPgA8AC8ARABBAFQAQQA+ADwALwBXAFIATQBIAEUAQQBEAEUAUgA+AA==' },
                                  { kid: [ 0x55, 0x8e, 0xe5, 0x41, 0xb9, 0x0a, 0xb2, 0xf3, 0x95, 0x0d, 0x00, 0xad, 0xe3, 0x76, 0x0d, 0x45 ],
                                    key: [ 0x91, 0x03, 0x92, 0x63, 0x01, 0x6d, 0xa6, 0x35, 0x77, 0x0d, 0x57, 0xdb, 0x92, 0xf9, 0x8b, 0xd0 ],
                                    initDataType : 'cenc',
                                    initData: 'AAAAcXBzc2gAAAAA7e+LqXnWSs6jyCfc1R0h7QAAAFEIARIQVY7lQbkKsvOVDQCt43YNRRoIY2FzdGxhYnMiKGV5SmhjM05sZEVsa0lqb2laVzFsTFhSbGMzUXRjMmx1WjJ4bEluMD0yB2RlZmF1bHQAAAMacHNzaAAAAACaBPB5mEBChquS5lvgiF+VAAAC+voCAAABAAEA8AI8AFcAUgBNAEgARQBBAEQARQBSACAAeABtAGwAbgBzAD0AIgBoAHQAdABwADoALwAvAHMAYwBoAGUAbQBhAHMALgBtAGkAYwByAG8AcwBvAGYAdAAuAGMAbwBtAC8ARABSAE0ALwAyADAAMAA3AC8AMAAzAC8AUABsAGEAeQBSAGUAYQBkAHkASABlAGEAZABlAHIAIgAgAHYAZQByAHMAaQBvAG4APQAiADQALgAwAC4AMAAuADAAIgA+ADwARABBAFQAQQA+ADwAUABSAE8AVABFAEMAVABJAE4ARgBPAD4APABLAEUAWQBMAEUATgA+ADEANgA8AC8ASwBFAFkATABFAE4APgA8AEEATABHAEkARAA+AEEARQBTAEMAVABSADwALwBBAEwARwBJAEQAPgA8AC8AUABSAE8AVABFAEMAVABJAE4ARgBPAD4APABLAEkARAA+AFEAZQBXAE8AVgBRAHEANQA4ADcASwBWAEQAUQBDAHQANAAzAFkATgBSAFEAPQA9ADwALwBLAEkARAA+ADwAQwBIAEUAQwBLAFMAVQBNAD4AWQBpAE8ALwAxADYATABzADkANgBFAD0APAAvAEMASABFAEMASwBTAFUATQA+ADwATABBAF8AVQBSAEwAPgBoAHQAdABwADoALwAvAHAAbABhAHkAcgBlAGEAZAB5AC4AZABpAHIAZQBjAHQAdABhAHAAcwAuAG4AZQB0AC8AcAByAC8AcwB2AGMALwByAGkAZwBoAHQAcwBtAGEAbgBhAGcAZQByAC4AYQBzAG0AeAA/AFAAbABhAHkAUgBpAGcAaAB0AD0AMQAmAGEAbQBwADsAVQBzAGUAUwBpAG0AcABsAGUATgBvAG4AUABlAHIAcwBpAHMAdABlAG4AdABMAGkAYwBlAG4AcwBlAD0AMQA8AC8ATABBAF8AVQBSAEwAPgA8AC8ARABBAFQAQQA+ADwALwBXAFIATQBIAEUAQQBEAEUAUgA+AA==' } ]
                    },

    'mp4-multikey' :  {     assetId:        'mp4-multikey',
                            initDataType:   'cenc',
                            audio:  {   type:   'audio/mp4;codecs="mp4a.40.2"',
                                        path:   '/encrypted-media/content/audio_aac-lc_128k_2keys_2sess.mp4' },
                            video:  {   type:   'video/mp4;codecs="avc1.4d401e"',
                                        path:   '/encrypted-media/content/video_512x288_h264-360k_enc_2keys_2sess.mp4' },
                            keys: [ {   kid:    [ 0x13, 0xa7, 0x53, 0x06, 0xd1, 0x18, 0x91, 0x7b, 0x47, 0xa6, 0xc1, 0x83, 0x64, 0x42, 0x51, 0x6f ],
                                        key:    [ 0x8a, 0xaa, 0xd8, 0xc4, 0xdb, 0xde, 0xac, 0xcd, 0xad, 0x26, 0x76, 0xa1, 0xed, 0x38, 0x95, 0x2e ],
                                        variantId:      'key1',
                                        initDataType:   'cenc',
                                        initData: 'AAAAjXBzc2gAAAAA7e+LqXnWSs6jyCfc1R0h7QAAAG0IARIQE6dTBtEYkXtHpsGDZEJRbxoIY2FzdGxhYnMiRGV5SmhjM05sZEVsa0lqb2laVzFsTFhSbGMzUXRNbk5sYzNOcGIyNGlMQ0oyWVhKcFlXNTBTV1FpT2lKclpYa3hJbjA9MgdkZWZhdWx0AAADwnBzc2gAAAAAmgTweZhAQoarkuZb4IhflQAAA6KiAwAAAQABAJgDPABXAFIATQBIAEUAQQBEAEUAUgAgAHgAbQBsAG4AcwA9ACIAaAB0AHQAcAA6AC8ALwBzAGMAaABlAG0AYQBzAC4AbQBpAGMAcgBvAHMAbwBmAHQALgBjAG8AbQAvAEQAUgBNAC8AMgAwADAANwAvADAAMwAvAFAAbABhAHkAUgBlAGEAZAB5AEgAZQBhAGQAZQByACIAIAB2AGUAcgBzAGkAbwBuAD0AIgA0AC4AMAAuADAALgAwACIAPgA8AEQAQQBUAEEAPgA8AFAAUgBPAFQARQBDAFQASQBOAEYATwA+ADwASwBFAFkATABFAE4APgAxADYAPAAvAEsARQBZAEwARQBOAD4APABBAEwARwBJAEQAPgBBAEUAUwBDAFQAUgA8AC8AQQBMAEcASQBEAD4APAAvAFAAUgBPAFQARQBDAFQASQBOAEYATwA+ADwASwBJAEQAPgBCAGwATwBuAEUAeABqAFIAZQA1AEYASABwAHMARwBEAFoARQBKAFIAYgB3AD0APQA8AC8ASwBJAEQAPgA8AEwAQQBfAFUAUgBMAD4AaAB0AHQAcABzADoALwAvAGwAaQBjAC4AcwB0AGEAZwBpAG4AZwAuAGQAcgBtAHQAbwBkAGEAeQAuAGMAbwBtAC8AbABpAGMAZQBuAHMAZQAtAHAAcgBvAHgAeQAtAGgAZQBhAGQAZQByAGEAdQB0AGgALwBkAHIAbQB0AG8AZABhAHkALwBSAGkAZwBoAHQAcwBNAGEAbgBhAGcAZQByAC4AYQBzAG0AeAA8AC8ATABBAF8AVQBSAEwAPgA8AEwAVQBJAF8AVQBSAEwAPgBoAHQAdABwAHMAOgAvAC8AbABpAGMALgBzAHQAYQBnAGkAbgBnAC4AZAByAG0AdABvAGQAYQB5AC4AYwBvAG0ALwBsAGkAYwBlAG4AcwBlAC0AcAByAG8AeAB5AC0AaABlAGEAZABlAHIAYQB1AHQAaAAvAGQAcgBtAHQAbwBkAGEAeQAvAFIAaQBnAGgAdABzAE0AYQBuAGEAZwBlAHIALgBhAHMAbQB4ADwALwBMAFUASQBfAFUAUgBMAD4APABDAEgARQBDAEsAUwBVAE0APgBJAEQAUgB0AFAAZwBVAEkALwBiAEkAPQA8AC8AQwBIAEUAQwBLAFMAVQBNAD4APAAvAEQAQQBUAEEAPgA8AC8AVwBSAE0ASABFAEEARABFAFIAPgA=' },
                                    {   kid:    [ 0xee, 0x73, 0x56, 0x4e, 0xc8, 0xa8, 0x90, 0xf0, 0x78, 0xef, 0x68, 0x71, 0xfa, 0x4b, 0xe1, 0x8b ],
                                        key:    [ 0xe4, 0x4f, 0xe1, 0x45, 0x7c, 0x5e, 0xbc, 0xd8, 0x3e, 0xad, 0xdc, 0xd6, 0x2c, 0xaf, 0x55, 0x18 ],
                                        variantId:      'key2',
                                        initDataType:   'cenc',
                                        initData: 'AAAAjXBzc2gAAAAA7e+LqXnWSs6jyCfc1R0h7QAAAG0IARIQ7nNWTsiokPB472hx+kvhixoIY2FzdGxhYnMiRGV5SmhjM05sZEVsa0lqb2laVzFsTFhSbGMzUXRNbk5sYzNOcGIyNGlMQ0oyWVhKcFlXNTBTV1FpT2lKclpYa3lJbjA9MgdkZWZhdWx0AAADwnBzc2gAAAAAmgTweZhAQoarkuZb4IhflQAAA6KiAwAAAQABAJgDPABXAFIATQBIAEUAQQBEAEUAUgAgAHgAbQBsAG4AcwA9ACIAaAB0AHQAcAA6AC8ALwBzAGMAaABlAG0AYQBzAC4AbQBpAGMAcgBvAHMAbwBmAHQALgBjAG8AbQAvAEQAUgBNAC8AMgAwADAANwAvADAAMwAvAFAAbABhAHkAUgBlAGEAZAB5AEgAZQBhAGQAZQByACIAIAB2AGUAcgBzAGkAbwBuAD0AIgA0AC4AMAAuADAALgAwACIAPgA8AEQAQQBUAEEAPgA8AFAAUgBPAFQARQBDAFQASQBOAEYATwA+ADwASwBFAFkATABFAE4APgAxADYAPAAvAEsARQBZAEwARQBOAD4APABBAEwARwBJAEQAPgBBAEUAUwBDAFQAUgA8AC8AQQBMAEcASQBEAD4APAAvAFAAUgBPAFQARQBDAFQASQBOAEYATwA+ADwASwBJAEQAPgBUAGwAWgB6ADcAcQBqAEkAOABKAEIANAA3ADIAaAB4ACsAawB2AGgAaQB3AD0APQA8AC8ASwBJAEQAPgA8AEwAQQBfAFUAUgBMAD4AaAB0AHQAcABzADoALwAvAGwAaQBjAC4AcwB0AGEAZwBpAG4AZwAuAGQAcgBtAHQAbwBkAGEAeQAuAGMAbwBtAC8AbABpAGMAZQBuAHMAZQAtAHAAcgBvAHgAeQAtAGgAZQBhAGQAZQByAGEAdQB0AGgALwBkAHIAbQB0AG8AZABhAHkALwBSAGkAZwBoAHQAcwBNAGEAbgBhAGcAZQByAC4AYQBzAG0AeAA8AC8ATABBAF8AVQBSAEwAPgA8AEwAVQBJAF8AVQBSAEwAPgBoAHQAdABwAHMAOgAvAC8AbABpAGMALgBzAHQAYQBnAGkAbgBnAC4AZAByAG0AdABvAGQAYQB5AC4AYwBvAG0ALwBsAGkAYwBlAG4AcwBlAC0AcAByAG8AeAB5AC0AaABlAGEAZABlAHIAYQB1AHQAaAAvAGQAcgBtAHQAbwBkAGEAeQAvAFIAaQBnAGgAdABzAE0AYQBuAGEAZwBlAHIALgBhAHMAbQB4ADwALwBMAFUASQBfAFUAUgBMAD4APABDAEgARQBDAEsAUwBVAE0APgB4AEQASwBBAFkAMAB2AFoAaABVAFUAPQA8AC8AQwBIAEUAQwBLAFMAVQBNAD4APAAvAEQAQQBUAEEAPgA8AC8AVwBSAE0ASABFAEEARABFAFIAPgA=' } ] },


    'mp4-multikey-sequential' : {   assetId:    'mp4-multikey-sequential',
                                    initDataType:   'cenc',
                                    audio: {    type:   'audio/mp4;codecs="mp4a.40.2"',
                                                path:   '/encrypted-media/content/audio_aac-lc_128k_dashinit.mp4' },
                                    video: {    type:   'video/mp4;codecs="avc1.4d401e"',
                                                path:   '/encrypted-media/content/video_512x288_h264-360k_multikey_dashinit.mp4' },
                                    keys: [ {   kid:    [0x8a, 0x0d, 0x85, 0x45, 0x21, 0x05, 0xd4, 0x15, 0x35, 0x8f, 0xea, 0x8f, 0x68, 0xe6, 0xc1, 0x91],
                                                key:    [0x76, 0x6f, 0xab, 0xc1, 0x68, 0x3f, 0xf8, 0xef, 0x4e, 0x76, 0x00, 0x24, 0xc5, 0x23, 0x8f, 0x10],
                                                variantId:      'key1',
                                                initDataType:   'cenc',
                                                initData: 'AAAAlXBzc2gAAAAA7e+LqXnWSs6jyCfc1R0h7QAAAHUIARIQig2FRSEF1BU1j+qPaObBkRoIY2FzdGxhYnMiTGV5SmhjM05sZEVsa0lqb2liWEEwTFcxMWJIUnBhMlY1TFhObGNYVmxiblJwWVd3aUxDSjJZWEpwWVc1MFNXUWlPaUpyWlhreEluMD0yB2RlZmF1bHQAAANYcHNzaAAAAACaBPB5mEBChquS5lvgiF+VAAADODgDAAABAAEALgM8AFcAUgBNAEgARQBBAEQARQBSACAAeABtAGwAbgBzAD0AIgBoAHQAdABwADoALwAvAHMAYwBoAGUAbQBhAHMALgBtAGkAYwByAG8AcwBvAGYAdAAuAGMAbwBtAC8ARABSAE0ALwAyADAAMAA3AC8AMAAzAC8AUABsAGEAeQBSAGUAYQBkAHkASABlAGEAZABlAHIAIgAgAHYAZQByAHMAaQBvAG4APQAiADQALgAwAC4AMAAuADAAIgA+ADwARABBAFQAQQA+ADwAUABSAE8AVABFAEMAVABJAE4ARgBPAD4APABLAEUAWQBMAEUATgA+ADEANgA8AC8ASwBFAFkATABFAE4APgA8AEEATABHAEkARAA+AEEARQBTAEMAVABSADwALwBBAEwARwBJAEQAPgA8AC8AUABSAE8AVABFAEMAVABJAE4ARgBPAD4APABLAEkARAA+AFIAWQBVAE4AaQBnAFUAaABGAGQAUQAxAGoAKwBxAFAAYQBPAGIAQgBrAFEAPQA9ADwALwBLAEkARAA+ADwATABBAF8AVQBSAEwAPgBoAHQAdABwAHMAOgAvAC8AbABpAGMALgBzAHQAYQBnAGkAbgBnAC4AZAByAG0AdABvAGQAYQB5AC4AYwBvAG0ALwBsAGkAYwBlAG4AcwBlAC0AcAByAG8AeAB5AC0AaABlAGEAZABlAHIAYQB1AHQAaAAvAGQAcgBtAHQAbwBkAGEAeQAvAFIAaQBnAGgAdABzAE0AYQBuAGEAZwBlAHIALgBhAHMAbQB4ADwALwBMAEEAXwBVAFIATAA+ADwATABVAEkAXwBVAFIATAA+AGgAdAB0AHAAcwA6AC8ALwBwAGwAYQB5AHIAZQBhAGQAeQAtAHUAaQAuAGUAeABhAG0AcABsAGUALgBjAG8AbQA8AC8ATABVAEkAXwBVAFIATAA+ADwAQwBIAEUAQwBLAFMAVQBNAD4AcQBOAEkAZQBiAFQAWABzAG8AcgBnAD0APAAvAEMASABFAEMASwBTAFUATQA+ADwALwBEAEEAVABBAD4APAAvAFcAUgBNAEgARQBBAEQARQBSAD4A' },
                                            {   kid:    [0xfb, 0xb4, 0xb7, 0xf3, 0x4a, 0xbd, 0x31, 0x87, 0x34, 0x4b, 0xce, 0xc4, 0x5f, 0x96, 0x68, 0x88],
                                                key:    [0x26, 0x52, 0xc3, 0x1d, 0xf7, 0x92, 0xd1, 0x7b, 0x08, 0xa6, 0xfa, 0xd3, 0x7c, 0xb6, 0x25, 0x60],
                                                variantId:      'key2',
                                                initDataType:   'cenc',
                                                initData: 'AAAAlXBzc2gAAAAA7e+LqXnWSs6jyCfc1R0h7QAAAHUIARIQ+7S380q9MYc0S87EX5ZoiBoIY2FzdGxhYnMiTGV5SmhjM05sZEVsa0lqb2liWEEwTFcxMWJIUnBhMlY1TFhObGNYVmxiblJwWVd3aUxDSjJZWEpwWVc1MFNXUWlPaUpyWlhreUluMD0yB2RlZmF1bHQAAANYcHNzaAAAAACaBPB5mEBChquS5lvgiF+VAAADODgDAAABAAEALgM8AFcAUgBNAEgARQBBAEQARQBSACAAeABtAGwAbgBzAD0AIgBoAHQAdABwADoALwAvAHMAYwBoAGUAbQBhAHMALgBtAGkAYwByAG8AcwBvAGYAdAAuAGMAbwBtAC8ARABSAE0ALwAyADAAMAA3AC8AMAAzAC8AUABsAGEAeQBSAGUAYQBkAHkASABlAGEAZABlAHIAIgAgAHYAZQByAHMAaQBvAG4APQAiADQALgAwAC4AMAAuADAAIgA+ADwARABBAFQAQQA+ADwAUABSAE8AVABFAEMAVABJAE4ARgBPAD4APABLAEUAWQBMAEUATgA+ADEANgA8AC8ASwBFAFkATABFAE4APgA8AEEATABHAEkARAA+AEEARQBTAEMAVABSADwALwBBAEwARwBJAEQAPgA8AC8AUABSAE8AVABFAEMAVABJAE4ARgBPAD4APABLAEkARAA+ADgANwBlADAAKwA3ADEASwBoAHoARQAwAFMAOAA3AEUAWAA1AFoAbwBpAEEAPQA9ADwALwBLAEkARAA+ADwATABBAF8AVQBSAEwAPgBoAHQAdABwAHMAOgAvAC8AbABpAGMALgBzAHQAYQBnAGkAbgBnAC4AZAByAG0AdABvAGQAYQB5AC4AYwBvAG0ALwBsAGkAYwBlAG4AcwBlAC0AcAByAG8AeAB5AC0AaABlAGEAZABlAHIAYQB1AHQAaAAvAGQAcgBtAHQAbwBkAGEAeQAvAFIAaQBnAGgAdABzAE0AYQBuAGEAZwBlAHIALgBhAHMAbQB4ADwALwBMAEEAXwBVAFIATAA+ADwATABVAEkAXwBVAFIATAA+AGgAdAB0AHAAcwA6AC8ALwBwAGwAYQB5AHIAZQBhAGQAeQAtAHUAaQAuAGUAeABhAG0AcABsAGUALgBjAG8AbQA8AC8ATABVAEkAXwBVAFIATAA+ADwAQwBIAEUAQwBLAFMAVQBNAD4ARgB0AGkASQBoADYAUwBKAG0AcABZAD0APAAvAEMASABFAEMASwBTAFUATQA+ADwALwBEAEEAVABBAD4APAAvAFcAUgBNAEgARQBBAEQARQBSAD4A' } ] },

    'webm' :        {   audio : {   type:   'audio/webm; codecs="opus"' },
                        video : {   type:   'video/webm; codecs="vp8"',
                                    path:   '/encrypted-media/content/test-encrypted.webm' },
                        keys :  [ { kid:    [48,49,50,51,52,53,54,55,56,57,48,49,50,51,52,53],
                                    key:    [0xeb, 0xdd, 0x62, 0xf1, 0x68, 0x14, 0xd2, 0x7b,
                                             0x68, 0xef, 0x12, 0x2a, 0xfc, 0xe4, 0xae, 0x3c ] } ]
                    },
    'webm-multikey' :
                    {   audio : {   type:   'audio/webm; codecs="opus"' },
                        video : {   type:   'video/webm; codecs="vp8"',
                                    path:   '/encrypted-media/content/test-encrypted-different-av-keys.webm' },
                        keys :  [ { kid:    [48,49,50,51,52,53,54,55,56,57,48,49,50,51,52,53],
                                    key:    [   0x7A, 0x7A, 0x62, 0xF1, 0x68, 0x14, 0xD2, 0x7B,
                                                0x68, 0xEF, 0x12, 0x2A, 0xFC, 0xE4, 0xAE, 0x0A ] },
                                  { kid:    [49,50,51,52,53,54,55,56,57,48,49,50,51,52,53,54],
                                    key:    [   0x30, 0x30, 0x62, 0xF1, 0x68, 0x14, 0xD2, 0x7B,
                                                0x68, 0xEF, 0x12, 0x2A, 0xFC, 0xE4, 0xAE, 0x0A ] } ]
                    },
} );

function addMemberListToObject( o )
{
    var items = [ ];
    for( var item in o )
    {
        if ( !o.hasOwnProperty( item ) ) continue;

        o[item].name = item;
        items.push( o[item] );
    }

    o._items = items;

    return o;
}

function getInitData( contentitem, initDataType )
{
    if (initDataType == 'webm') {
      return new Uint8Array( contentitem.keys[ 0 ].kid );       // WebM initData supports only a single key
    }

    if (initDataType == 'cenc') {

        var size = 36 + contentitem.keys.length * 16,
            kids = contentitem.keys.map( function( k ) { return k.kid; } );

        return new Uint8Array(Array.prototype.concat.call( [
            0x00, 0x00, size / 256, size % 256, // size
            0x70, 0x73, 0x73, 0x68, // 'pssh'
            0x01, // version = 1
            0x00, 0x00, 0x00, // flags
            0x10, 0x77, 0xEF, 0xEC, 0xC0, 0xB2, 0x4D, 0x02, // Common SystemID
            0xAC, 0xE3, 0x3C, 0x1E, 0x52, 0xE2, 0xFB, 0x4B,
            0x00, 0x00, 0x00, kids.length ], // key count ]
            Array.prototype.concat.apply( [], kids ),
          [ 0x00, 0x00, 0x00, 0x00 ]// datasize
        ));
    }
    if (initDataType == 'keyids') {

        return toUtf8( { kids: contentitem.keys.map( function( k ) { return base64urlEncode( new Uint8Array( k.kid ) ); } ) } );
    }
    throw 'initDataType ' + initDataType + ' not supported.';
}

function getSingleKeyInitData( kid, initDataType )
{
    if (initDataType == 'webm') {
      return new Uint8Array( kid );
    }

    if (initDataType == 'cenc') {

        var size = 52;

        return new Uint8Array(Array.prototype.concat.call( [
            0x00, 0x00, size / 256, size % 256, // size
            0x70, 0x73, 0x73, 0x68, // 'pssh'
            0x01, // version = 1
            0x00, 0x00, 0x00, // flags
            0x10, 0x77, 0xEF, 0xEC, 0xC0, 0xB2, 0x4D, 0x02, // Common SystemID
            0xAC, 0xE3, 0x3C, 0x1E, 0x52, 0xE2, 0xFB, 0x4B,
            0x00, 0x00, 0x00, 0x01 ], // key count ]
            kid,
          [ 0x00, 0x00, 0x00, 0x00 ]// datasize
        ));
    }
    if (initDataType == 'keyids') {

        return toUtf8( { kids: [ base64urlEncode( new Uint8Array( kid ) ) ] } );
    }
    throw 'initDataType ' + initDataType + ' not supported.';
}

function getMultikeyInitDatas( contentitem, initDataType )
{
    return contentitem.keys.map( function( k ) { return getSingleKeyInitData( k.kid, initDataType ); } );
}

function getProprietaryInitDatas( contentitem )
{
    var keysWithInitData = contentitem.keys.filter( function( k ) { return k.initData; } );
    return { initDataType: contentitem.initDataType,
             initDatas : keysWithInitData.map( function( k ) { return k.initData; } ),
             variantIds: keysWithInitData.map( function( k ) { return k.variantId; } )
            };
}

// Returns a promise that resolves to the following object
// { supported: boolean,                        // whether the content is supported at all
//      content: <the content item>,            // the content item description
//      initDataTypes: <list of initDataTypes>
// }
//
// Note: we test initData types one at a time since some versions of Edge don't support testing several at once
//
function isContentSupportedForInitDataTypes( keysystem, initDataTypes, contentitem )
{
    return Promise.all( initDataTypes.map( function( initDataType ) {
        var configuration = {   initDataTypes : [ initDataType ],
                              audioCapabilities: [ { contentType: contentitem.audio.type } ],
                              videoCapabilities: [ { contentType: contentitem.video.type } ]
                          };
        return navigator.requestMediaKeySystemAccess( keysystem, [ configuration ] ).then( function( access ) {
            return { supported: true, initDataType: access.getConfiguration().initDataTypes[ 0 ] };
        }, function() {
            return { supported: false };
        } );
    } ) ).then( function( results ) {

        var initDataTypes = results.filter( function( result ) { return result.supported; } )
                                    .map( function( result ) { return result.initDataType; } );

        return initDataTypes.length > 0 ?
                    { content: contentitem, supported: true, initDataTypes: initDataTypes }
                    : { content: contentitem, supported: false };
    } );
}

// Returns a promise that resolves to { content:, supported:, initDataTypes: } object
function isContentSupported( keysystem, contentitem )
{
    return isContentSupportedForInitDataTypes( keysystem, [ 'cenc', 'webm', 'keyids' ], contentitem );
}

// Returns a Promise resolving to an array of supported content for the key system
function getSupportedContent( keysystem )
{
    return Promise.all( content._items.map( isContentSupported.bind( null, keysystem ) ) ).
    then( function( results )
    {
        return results.filter( function( r ) { return r.supported; } ).map( function( r ) { return r.content; } );
    } );
}

// Returns a Promise resolving to an array of { content:, initDataType: } pairs for the key system
function getSupportedContentAndInitDataTypes( keysystem )
{
    return Promise.all( content._items.map( isContentSupported.bind( null, keysystem ) ) ).
    then( function( results )
    {
        return results.filter( function( r ) { return r.supported; } );
    } );
}

// gets a configuration object for provided piece of content
function getSimpleConfigurationForContent( contentitem )
{
    return {    initDataTypes: [ 'keyids', 'webm', 'cenc' ],
                audioCapabilities: [ { contentType: contentitem.audio.type } ],
                videoCapabilities: [ { contentType: contentitem.video.type } ] };
}
