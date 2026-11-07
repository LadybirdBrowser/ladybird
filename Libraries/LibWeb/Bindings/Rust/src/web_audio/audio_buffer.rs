/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(
    clippy::needless_range_loop,
    clippy::collapsible_if,
    reason = "preserve the C++ channel and cache traversal order"
)]

use std::cell::RefCell;
use std::ptr::NonNull;
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::{
    ArrayBuffer, ArrayBufferOrder, ArrayBufferShared, DataBlock, EmbedderCell, ErrorKind, ErrorType,
    ExternalPrimitiveStorage, ForeignCellSlot, Gc, GcWeak, Trace, TypedArrayBase, TypedArrayKind, Vm,
    create_typed_array_on_buffer, detach_array_buffer, make_typed_array_with_buffer_witness_record,
};
use libjs_rust::api::{OutOfMemory, OwnedBackingStore};
use libjs_rust::api::{is_typed_array_out_of_bounds, typed_array_byte_length};
use libweb_rust::web_audio::rendering::audio_data::AudioBufferContents;

use crate::bindings::interface::{Ref, Wrappable, Wrapped, allocate, cell_into_abi};
use crate::bindings::{CallContext, Exception, WebIdlResult};

pub(crate) struct AudioBufferOptions {
    pub(crate) number_of_channels: u32,
    pub(crate) length: u32,
    pub(crate) sample_rate: f32,
}

// The sample data lives in the primitive storage cage, so an out-of-bounds access through a channel's Float32Array
// is masked back into the cage instead of reaching memory outside it. The AudioBuffer owns the storage, and the
// ArrayBuffer handed to script holds an owner edge back to the AudioBuffer, so the storage outlives every view.
struct Channel {
    data: OwnedBackingStore,
}

// https://webaudio.github.io/web-audio-api/#AudioBuffer
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AudioBuffer {
    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-number-of-channels-slot
    // The number of audio channels for this AudioBuffer, which is an unsigned long.
    //
    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-internal-data-slot
    // A data block holding the audio sample data.
    #[gc(untraced)]
    channels: RefCell<Vec<Channel>>, // [[internal data]] / [[number_of_channels]]
    // https://webaudio.github.io/web-audio-api/#audio-buffer-copying
    #[gc(untraced)]
    contents: RefCell<Option<Arc<AudioBufferContents>>>,

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-length-slot
    // The length of each channel of this AudioBuffer, which is an unsigned long.
    length: u32, // [[length]]
    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-sample-rate-slot
    // The sample-rate, in Hz, of this AudioBuffer, a float.
    sample_rate: f32, // [[sample rate]]
}

impl Wrappable for AudioBuffer {
    const OWNS_EXTERNAL_MEMORY: bool = true;

    fn external_memory_size(&self) -> usize {
        let channels = self.channels.borrow();
        let mut size = channels.capacity() * size_of::<Channel>();
        for channel in &*channels {
            size = size.saturating_add(channel.data.capacity());
        }
        size
    }
}

// The ArrayBuffer is tracked next to the view because it can outlive it: script is free to keep `array.buffer` and
// drop the array. The buffer is what projects the channel's storage, so it is the one that decides how long that
// storage must stay put, and the one that has to be detached when the content is acquired.
struct AudioBufferChannelView {
    view: GcWeak<TypedArrayBase>,
    array_buffer: GcWeak<ArrayBuffer>,
}

struct AudioBufferChannelDataCacheEntry {
    buffer: GcWeak<EmbedderCell<Wrapped<AudioBuffer>>>,
    channel: u32,
    views: Vec<AudioBufferChannelView>,
}

thread_local! { static AUDIO_BUFFER_CHANNEL_DATA_CACHES: RefCell<Vec<AudioBufferChannelDataCacheEntry>> = const { RefCell::new(Vec::new()) }; }
fn prune_audio_buffer_channel_data_caches(caches: &mut Vec<AudioBufferChannelDataCacheEntry>) {
    caches.retain(|entry| entry.buffer.get().is_some());
}

fn cache_for<'a>(
    caches: &'a mut Vec<AudioBufferChannelDataCacheEntry>,
    vm: &Vm,
    buffer: Ref<AudioBuffer>,
    channel: u32,
) -> &'a mut AudioBufferChannelDataCacheEntry {
    prune_audio_buffer_channel_data_caches(caches);
    for index in 0..caches.len() {
        if caches[index].buffer.get() == Some(buffer) && caches[index].channel == channel {
            return &mut caches[index];
        }
    }
    caches.push(AudioBufferChannelDataCacheEntry {
        buffer: GcWeak::new(vm.heap(), buffer),
        channel,
        views: Vec::new(),
    });
    caches.last_mut().unwrap()
}

fn prune_live_channel_views(views: &mut Vec<AudioBufferChannelView>) {
    views.retain(|entry| entry.array_buffer.get().is_some());
}

impl AudioBuffer {
    pub(crate) fn construct(cx: &mut CallContext, options: AudioBufferOptions) -> WebIdlResult<Ref<Self>> {
        let vm = cx.vm();

        // 1. If any of the values in options lie outside its nominal range, throw a NotSupportedError exception and abort the following steps.
        super::base_audio_context::BaseAudioContext::verify_audio_options_inside_nominal_range(
            options.number_of_channels,
            options.length,
            options.sample_rate,
        )?;
        if options.length as usize > i32::MAX as usize / size_of::<f32>() {
            return Err(vm
                .throw_completion::<()>(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"typed array"])
                .unwrap_err()
                .into());
        }

        // 2. Let b be a new AudioBuffer object.
        // 3. Respectively assign the values of the attributes numberOfChannels, length, sampleRate of the AudioBufferOptions passed in the
        //    constructor to the internal slots [[number of channels]], [[length]], [[sample rate]].
        // 4. Set the internal slot [[internal data]] of this AudioBuffer to the result of calling CreateByteDataBlock([[length]] * [[number of channels]]).
        Self::create(vm, options.number_of_channels, options.length, options.sample_rate).map_err(|_| {
            vm.throw_completion::<()>(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                .unwrap_err()
                .into()
        })
    }

    pub(crate) fn create(
        vm: &Vm,
        number_of_channels: u32,
        length: u32,
        sample_rate: f32,
    ) -> Result<Ref<Self>, OutOfMemory> {
        let buffer = allocate(vm, |_| Self {
            channels: RefCell::new(Vec::new()),
            contents: RefCell::new(None),
            length,
            sample_rate,
        });
        let channel_byte_length = length as usize * size_of::<f32>();
        let mut channels = buffer.channels.borrow_mut();
        channels
            .try_reserve_exact(number_of_channels as usize)
            .map_err(|_| OutOfMemory)?;
        for _i in 0..number_of_channels {
            channels.push(Channel {
                data: OwnedBackingStore::create_zeroed(channel_byte_length)?,
            });
        }
        drop(channels);
        vm.heap().did_allocate_external_memory(buffer.external_memory_size());
        Ok(buffer)
    }

    // https://webaudio.github.io/web-audio-api/#acquire-the-content
    pub(crate) fn acquire_contents(this: Ref<Self>, vm: &Vm) -> Option<Arc<AudioBufferContents>> {
        // NB: contents is populated exactly while the channel arrays are detached because step 4 below has not run yet.
        //     The snapshot then still holds the buffer's content and script has had no way to modify it, so it can be
        //     handed out again as-is. This is what makes a sequence of acquisitions with no intervening getChannelData(),
        //     e.g. one AudioBuffer played by many AudioBufferSourceNodes, free of allocations and copying.
        //     https://webaudio.github.io/web-audio-api/#audio-buffer-copying
        if let Some(contents) = &*this.contents.borrow() {
            return Some(Arc::clone(contents));
        }

        // 1. If any of the AudioBuffer's ArrayBuffers are detached, return true, abort these steps, and return a
        //    zero-length channel data buffer to the invoker.
        let detached = AUDIO_BUFFER_CHANNEL_DATA_CACHES.with_borrow(|caches| {
            for cache in caches {
                if cache.buffer.get() != Some(this) {
                    continue;
                }
                for entry in &cache.views {
                    if entry.array_buffer.get().is_some_and(|buffer| buffer.is_detached()) {
                        return true;
                    }
                }
            }
            false
        });
        if detached {
            return None;
        }

        // 2. Detach all ArrayBuffers for arrays previously returned by getChannelData() on this AudioBuffer.
        // 3. Retain the underlying [[internal data]] from those ArrayBuffers and return references to them to the invoker.
        let mut channels = Vec::with_capacity(this.channels.borrow().len());
        for channel in &*this.channels.borrow() {
            let mut samples = vec![0_f32; this.length as usize];
            if !samples.is_empty() {
                unsafe {
                    std::ptr::copy_nonoverlapping(
                        channel.data.data(),
                        samples.as_mut_ptr().cast::<u8>(),
                        samples.len() * size_of::<f32>(),
                    );
                }
            }
            channels.push(samples);
        }
        AUDIO_BUFFER_CHANNEL_DATA_CACHES.with_borrow(|caches| {
            for cache in caches {
                if cache.buffer.get() != Some(this) {
                    continue;
                }
                for entry in &cache.views {
                    if let Some(array_buffer) = entry.array_buffer.get() {
                        detach_array_buffer(vm, array_buffer, None).expect("channel ArrayBuffers can be detached");
                    }
                }
            }
        });
        *this.contents.borrow_mut() = Some(Arc::new(AudioBufferContents {
            channels,
            sample_rate: this.sample_rate,
        }));
        vm.heap()
            .did_allocate_external_memory(this.channels.borrow().len() * this.length as usize * size_of::<f32>());

        // 4. Attach ArrayBuffers containing copies of the data to the AudioBuffer, to be returned by the next call to
        //    getChannelData().
        // NB: Performed lazily by attach_acquired_channels().
        this.contents.borrow().clone()
    }

    // https://webaudio.github.io/web-audio-api/#audio-buffer-copying
    pub(crate) fn attach_acquired_channels(this: Ref<Self>) {
        // The pending final step of acquiring this buffer's content: fresh arrays holding a copy of the acquired sample
        // data are attached the first time script reaches for the channel data again. Whoever acquired the content keeps
        // using the snapshot, so writes through the new arrays cannot change what is already playing.
        let mut contents = this.contents.borrow_mut();
        let Some(snapshot) = &*contents else {
            return;
        };
        let channels = this.channels.borrow();
        assert_eq!(snapshot.channels.len(), channels.len());
        for i in 0..channels.len() {
            let samples = &snapshot.channels[i];
            if !samples.is_empty() {
                unsafe {
                    std::ptr::copy_nonoverlapping(
                        samples.as_ptr().cast::<u8>(),
                        channels[i].data.data(),
                        samples.len() * size_of::<f32>(),
                    );
                }
            }
        }
        *contents = None;

        // Every view of the storage was detached when the content was acquired, and the storage needs fresh views over it.
        // So, drop the cached ones instead of handing a detached array back to the next caller.
        AUDIO_BUFFER_CHANNEL_DATA_CACHES.with_borrow_mut(|caches| {
            for cache in caches {
                if cache.buffer.get() == Some(this) {
                    cache.views.clear();
                }
            }
        });
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-samplerate
    pub(crate) fn sample_rate(&self, _: &mut CallContext) -> WebIdlResult<f32> {
        // The sample-rate for the PCM audio data in samples per second. This MUST return the value of [[sample rate]].
        Ok(self.sample_rate)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-length
    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        // Length of the PCM audio data in sample-frames. This MUST return the value of [[length]].
        Ok(self.length)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-duration
    pub(crate) fn duration(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // Duration of the PCM audio data in seconds.
        // This is computed from the [[sample rate]] and the [[length]] of the AudioBuffer by performing a division between the [[length]] and the [[sample rate]].
        Ok(f64::from(self.length) / f64::from(self.sample_rate))
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-numberofchannels
    pub(crate) fn number_of_channels(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        // The number of discrete audio channels. This MUST return the value of [[number of channels]].
        Ok(self.channels.borrow().len() as u32)
    }

    pub(crate) fn channel_data(&self, channel: u32) -> WebIdlResult<(*mut u8, usize)> {
        let channels = self.channels.borrow();
        if channel as usize >= channels.len() {
            return Err(Exception::index_size_error(Utf16String::from_ascii(
                b"Channel index is out of range",
            )));
        }
        let data = &channels[channel as usize].data;
        Ok((data.data(), data.size()))
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-getchanneldata
    pub(crate) fn get_channel_data(
        this: &Ref<Self>,
        cx: &mut CallContext,
        channel: u32,
    ) -> WebIdlResult<Gc<TypedArrayBase>> {
        if channel as usize >= this.channels.borrow().len() {
            return Err(Exception::index_size_error(Utf16String::from_ascii(
                b"Channel index is out of range",
            )));
        }

        // This is where the deferred final step of acquiring the content runs: script is reaching for the channel data
        // again, so the copies of the acquired samples become this buffer's internal data before anything reads them.
        Self::attach_acquired_channels(*this);
        let realm = cx.realm();
        AUDIO_BUFFER_CHANNEL_DATA_CACHES.with_borrow_mut(|caches| {
            let cache = cache_for(caches, cx.vm(), *this, channel);
            prune_live_channel_views(&mut cache.views);
            for entry in &cache.views {
                if let Some(view) = entry.view.get() {
                    if view.shape().realm() == realm {
                        return Ok(view);
                    }
                }
            }

            // The ArrayBuffer projects this AudioBuffer's channel storage instead of owning a copy of it, and names the
            // AudioBuffer as the owner of that storage. The GC then traces the AudioBuffer through the ArrayBuffer, so the
            // samples cannot be freed while script still holds a view of them.
            let channels = this.channels.borrow();
            let channel_storage = &channels[channel as usize].data;
            let owner = ForeignCellSlot::empty();
            unsafe {
                owner.set(NonNull::new(cell_into_abi(*this)));
            }
            let storage = ExternalPrimitiveStorage::new(owner, channel_storage.handle(), Some(channel_storage.size()));
            let array_buffer = ArrayBuffer::create_from_data_block(
                cx.vm(),
                realm,
                DataBlock::external(storage, ArrayBufferShared::No),
            );
            let view = create_typed_array_on_buffer(
                cx.vm(),
                realm,
                TypedArrayKind::Float32Array,
                this.length as usize,
                array_buffer,
            );
            cache.views.try_reserve(1).map_err(|_| {
                cx.vm()
                    .throw_completion::<()>(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                    .unwrap_err()
            })?;
            cache.views.push(AudioBufferChannelView {
                view: GcWeak::new(cx.vm().heap(), view),
                array_buffer: GcWeak::new(cx.vm().heap(), array_buffer),
            });
            Ok(view)
        })
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-copyfromchannel
    pub(crate) fn copy_from_channel(
        &self,
        cx: &mut CallContext,
        destination: Gc<TypedArrayBase>,
        channel_number: u32,
        buffer_offset: u32,
    ) -> WebIdlResult<()> {
        // The copyFromChannel() method copies the samples from the specified channel of the AudioBuffer to the destination array.
        //
        // Let buffer be the AudioBuffer with Nb frames, let Nf be the number of elements in the destination array, and k be the value
        // of bufferOffset. Then the number of frames copied from buffer to destination is max(0,min(Nb−k,Nf)). If this is less than Nf,
        // then the remaining elements of destination are not modified.
        let vm = cx.vm();
        if destination.viewed_array_buffer().is_shared_array_buffer() {
            return Err(vm
                .throw_completion::<()>(ErrorKind::TypeError, ErrorType::SharedArrayBuffer, &[&"Float32Array"])
                .unwrap_err()
                .into());
        }
        if destination.viewed_array_buffer().is_detached() {
            return Ok(());
        }
        let channels = self.channels.borrow();
        if channel_number as usize >= channels.len() {
            return Err(Exception::index_size_error(Utf16String::from_ascii(
                b"Channel index is out of range",
            )));
        }
        let channel = &channels[channel_number as usize];
        let channel_length = self.length as usize;
        if buffer_offset as usize >= channel_length {
            return Ok(());
        }
        let destination_record = make_typed_array_with_buffer_witness_record(&destination, ArrayBufferOrder::SeqCst);
        let destination_length = if is_typed_array_out_of_bounds(&destination_record) {
            0
        } else {
            typed_array_byte_length(&destination_record) / size_of::<f32>()
        };
        let count = destination_length.min(channel_length - buffer_offset as usize);
        let destination_bytes = destination.viewed_array_buffer();
        if count != 0 {
            destination_bytes.with_data_block(|destination_block| unsafe {
                std::ptr::copy(
                    channel
                        .data
                        .data()
                        .wrapping_add(buffer_offset as usize * size_of::<f32>()),
                    destination_block.data_at(destination.byte_offset()),
                    count * size_of::<f32>(),
                );
            });
        }
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-copytochannel
    pub(crate) fn copy_to_channel(
        this: &Ref<Self>,
        cx: &mut CallContext,
        source: Gc<TypedArrayBase>,
        channel_number: u32,
        buffer_offset: u32,
    ) -> WebIdlResult<()> {
        // The copyToChannel() method copies the samples to the specified channel of the AudioBuffer from the source array.
        //
        // A UnknownError may be thrown if source cannot be copied to the buffer.
        //
        // Let buffer be the AudioBuffer with Nb frames, let Nf be the number of elements in the source array, and k be the value
        // of bufferOffset. Then the number of frames copied from source to the buffer is max(0,min(Nb−k,Nf)). If this is less than Nf,
        // then the remaining elements of buffer are not modified.
        let vm = cx.vm();
        if source.viewed_array_buffer().is_shared_array_buffer() {
            return Err(vm
                .throw_completion::<()>(ErrorKind::TypeError, ErrorType::SharedArrayBuffer, &[&"Float32Array"])
                .unwrap_err()
                .into());
        }
        if source.viewed_array_buffer().is_detached() {
            return Ok(());
        }
        if channel_number as usize >= this.channels.borrow().len() {
            return Err(Exception::index_size_error(Utf16String::from_ascii(
                b"Channel index is out of range",
            )));
        }
        Self::attach_acquired_channels(*this);
        let channels = this.channels.borrow();
        let channel = &channels[channel_number as usize];
        let channel_length = this.length as usize;
        if buffer_offset as usize >= channel_length {
            return Ok(());
        }
        let source_record = make_typed_array_with_buffer_witness_record(&source, ArrayBufferOrder::SeqCst);
        let source_length = if is_typed_array_out_of_bounds(&source_record) {
            0
        } else {
            typed_array_byte_length(&source_record) / size_of::<f32>()
        };
        let count = source_length.min(channel_length - buffer_offset as usize);
        let source_bytes = source.viewed_array_buffer();
        if count != 0 {
            source_bytes.with_data_block(|source_block| unsafe {
                std::ptr::copy(
                    source_block.data_at(source.byte_offset()),
                    channel
                        .data
                        .data()
                        .wrapping_add(buffer_offset as usize * size_of::<f32>()),
                    count * size_of::<f32>(),
                );
            });
        }
        Ok(())
    }
}
