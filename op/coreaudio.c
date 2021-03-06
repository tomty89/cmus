/*
 * Copyright (C) 2015 Yue Wang <yuleopen@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <AudioUnit/AudioUnit.h>
#include <AudioUnit/AUComponent.h>
#include <CoreAudio/CoreAudio.h>

#include "../debug.h"
#include "../op.h"
#include "../mixer.h"
#include "../sf.h"
#include "../utils.h"
#include "../xmalloc.h"

static char *coreaudio_opt_device_name     = NULL;
static bool  coreaudio_opt_enable_hog_mode = false;
static bool  coreaudio_opt_sync_rate       = false;

static int coreaudio_max_volume = 100;
static AudioDeviceID coreaudio_device_id = kAudioDeviceUnknown;
static AudioStreamBasicDescription coreaudio_format_description;
static AudioUnit coreaudio_audio_unit = NULL;
static UInt32 coreaudio_buffer_size = 0;
static char *coreaudio_buffer = NULL;
static UInt32 coreaudio_stereo_channels[2];
static int coreaudio_mixer_pipe_in = 0;
static int coreaudio_mixer_pipe_out = 0;
static pthread_mutex_t mutex;
static pthread_cond_t cond;
static bool stopping = false;
static bool dropping = false;
/* static bool blocking = false; */
static bool partial = false;

static OSStatus coreaudio_device_volume_change_listener(AudioObjectID inObjectID,
							UInt32 inNumberAddresses,
							const AudioObjectPropertyAddress inAddresses[],
							void *inClientData)
{
	notify_via_pipe(coreaudio_mixer_pipe_in);
	return noErr;
}

static OSStatus coreaudio_play_callback(void *user_data,
					AudioUnitRenderActionFlags *flags,
					const AudioTimeStamp *ts,
					UInt32 busnum,
					UInt32 nframes,
					AudioBufferList *buflist)
{
	bool locked;
	bool ret = true;
	struct timeval stop;

	d_print("stopping(pre): %d\n", stopping);

	locked = stopping ? 0 : !pthread_mutex_lock(&mutex);

	d_print("pre-wait: %d\n", locked);

	gettimeofday(&stop, NULL);
	d_print("time: %ld\n", (long) stop.tv_usec);
	if (locked) {
		coreaudio_buffer = buflist->mBuffers[0].mData;
		coreaudio_buffer_size = buflist->mBuffers[0].mDataByteSize;
		/* blocking = true; */
		pthread_cond_signal(&cond);
		pthread_cond_wait(&cond, &mutex);
		/* blocking = false; */
		ret = dropping; // after unblocked
		dropping = false; // asynchronous
		locked = !!pthread_mutex_unlock(&mutex);
	}

	d_print("post-unlock: %d\n", locked);

	d_print("stopping(post): %d\n", stopping);

	d_print("ret: %d\n", ret);
	if (ret)
		return kAudioUnitErr_NoConnection;
	else
		return noErr;
}

static AudioDeviceID coreaudio_get_default_device()
{
	AudioObjectPropertyAddress aopa = {
		kAudioHardwarePropertyDefaultOutputDevice,
		kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMaster
	};

	AudioDeviceID dev_id = kAudioDeviceUnknown;
	UInt32 dev_id_size = sizeof(dev_id);
	AudioObjectGetPropertyData(kAudioObjectSystemObject,
				   &aopa,
				   0,
				   NULL,
				   &dev_id_size,
				   &dev_id);
	return dev_id;
}

static AudioDeviceID coreaudio_find_device(const char *dev_name)
{
	AudioObjectPropertyAddress aopa = {
		kAudioHardwarePropertyDevices,
		kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMaster
	};

	UInt32 property_size = 0;
	OSStatus err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
						      &aopa,
						      0,
						      NULL,
						      &property_size);
	if (err != noErr)
		return kAudioDeviceUnknown;

	aopa.mSelector = kAudioHardwarePropertyDevices;
	int device_count = property_size / sizeof(AudioDeviceID);
	AudioDeviceID devices[device_count];
	property_size = sizeof(devices);

	err = AudioObjectGetPropertyData(kAudioObjectSystemObject,
					 &aopa,
					 0,
					 NULL,
					 &property_size,
					 devices);
	if (err != noErr)
		return kAudioDeviceUnknown;

	aopa.mSelector = kAudioDevicePropertyDeviceName;
	for (int i = 0; i < device_count; i++) {
		char name[256] = {0};
		property_size = sizeof(name);
		err = AudioObjectGetPropertyData(devices[i],
						 &aopa,
						 0,
						 NULL,
						 &property_size,
						 name);
		if (err == noErr && strcmp(name, dev_name) == 0) {
			return devices[i];
		}
	}

	return kAudioDeviceUnknown;
}

static const struct {
	channel_position_t pos;
	const AudioChannelLabel label;
} coreaudio_channel_mapping[] = {
	{ CHANNEL_POSITION_LEFT,                        kAudioChannelLabel_Left },
	{ CHANNEL_POSITION_RIGHT,                       kAudioChannelLabel_Right },
	{ CHANNEL_POSITION_CENTER,                      kAudioChannelLabel_Center },
	{ CHANNEL_POSITION_LFE,                         kAudioChannelLabel_LFEScreen },
	{ CHANNEL_POSITION_SIDE_LEFT,                   kAudioChannelLabel_LeftSurround },
	{ CHANNEL_POSITION_SIDE_RIGHT,                  kAudioChannelLabel_RightSurround },
	{ CHANNEL_POSITION_MONO,                        kAudioChannelLabel_Mono },
	{ CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,        kAudioChannelLabel_LeftCenter },
	{ CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,       kAudioChannelLabel_RightCenter },
	{ CHANNEL_POSITION_REAR_LEFT,                   kAudioChannelLabel_LeftSurroundDirect },
	{ CHANNEL_POSITION_REAR_RIGHT,                  kAudioChannelLabel_RightSurroundDirect },
	{ CHANNEL_POSITION_REAR_CENTER,                 kAudioChannelLabel_CenterSurround },
	{ CHANNEL_POSITION_INVALID,                     kAudioChannelLabel_Unknown },
};

static void coreaudio_set_channel_position(AudioDeviceID dev_id,
					    int channels,
					    const channel_position_t *map)
{
	AudioObjectPropertyAddress aopa = {
		kAudioDevicePropertyPreferredChannelLayout,
		kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMaster
	};
	AudioChannelLayout *layout = NULL;
	size_t layout_size = (size_t) &layout->mChannelDescriptions[channels];
	layout = (AudioChannelLayout*)malloc(layout_size);
	layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions ;
	layout->mChannelBitmap = 0;
	layout->mNumberChannelDescriptions = channels;
	AudioChannelDescription *descriptions = layout->mChannelDescriptions;
	for (int i = 0; i < channels; i++) {
		const channel_position_t pos = map[i];
		AudioChannelLabel label = kAudioChannelLabel_Mono;
		for (int j = 0; j < N_ELEMENTS(coreaudio_channel_mapping); j++) {
			if (pos == coreaudio_channel_mapping[j].pos) {
				label = coreaudio_channel_mapping[j].label;
				break;
			}
		}
		descriptions[channels - 1 - i].mChannelLabel = label;
		descriptions[i].mChannelFlags = kAudioChannelFlags_AllOff;
		descriptions[i].mCoordinates[0] = 0;
		descriptions[i].mCoordinates[1] = 0;
		descriptions[i].mCoordinates[2] = 0;
	}
	OSStatus err =
		AudioObjectSetPropertyData(dev_id,
				&aopa,
			 0, NULL, layout_size, layout);
	if (err != noErr)
		d_print("Cannot set the channel layout successfully.\n");
	free(layout);
}


static AudioStreamBasicDescription coreaudio_fill_format_description(sample_format_t sf)
{
	AudioStreamBasicDescription desc = {
		.mSampleRate       = (Float64)sf_get_rate(sf),
		.mFormatID         = kAudioFormatLinearPCM,
		.mFormatFlags      = kAudioFormatFlagIsPacked,
		.mBytesPerPacket   = sf_get_frame_size(sf),
		.mFramesPerPacket  = 1,
		.mChannelsPerFrame = sf_get_channels(sf),
		.mBitsPerChannel   = sf_get_bits(sf),
		.mBytesPerFrame    = sf_get_frame_size(sf),
	};

	d_print("Bits:%d\n", sf_get_bits(sf));
	if (sf_get_bigendian(sf))
		desc.mFormatFlags |= kAudioFormatFlagIsBigEndian;
	if (sf_get_signed(sf))
		desc.mFormatFlags |= kLinearPCMFormatFlagIsSignedInteger;

	return desc;
}

static void coreaudio_sync_device_sample_rate(AudioDeviceID dev_id, AudioStreamBasicDescription desc)
{
	AudioObjectPropertyAddress aopa = {
		kAudioDevicePropertyAvailableNominalSampleRates,
		kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMaster
	};

	UInt32 property_size;
	OSStatus err = AudioObjectGetPropertyDataSize(dev_id,
						      &aopa,
						      0,
						      NULL,
						      &property_size);

	int count = property_size/sizeof(AudioValueRange);
	AudioValueRange ranges[count];
	property_size = sizeof(ranges);
	err = AudioObjectGetPropertyData(dev_id,
					 &aopa,
					 0,
					 NULL,
					 &property_size,
					 &ranges);
	// Get the maximum sample rate as fallback.
	Float64 sample_rate = .0;
	for (int i = 0; i < count; i++) {
		if (ranges[i].mMaximum > sample_rate)
			sample_rate = ranges[i].mMaximum;
	}

	// Now try to see if the device support our format sample rate.
	// For some high quality media samples, the frame rate may exceed
	// device capability. In this case, we let CoreAudio downsample
	// by decimation with an integer factor ranging from 1 to 4.
	for (int f = 4; f > 0; f--) {
		Float64 rate = desc.mSampleRate / f;
		for (int i = 0; i < count; i++) {
			if (ranges[i].mMinimum <= rate
			   && rate <= ranges[i].mMaximum) {
				sample_rate = rate;
				break;
			}
		}
	}

	aopa.mSelector = kAudioDevicePropertyNominalSampleRate,

	err = AudioObjectSetPropertyData(dev_id,
					 &aopa,
					 0,
					 NULL,
					 sizeof(&desc.mSampleRate),
					 &sample_rate);
	if (err != noErr)
		d_print("Failed to synchronize the sample rate: %d\n", err);
}

static void coreaudio_hog_device(AudioDeviceID dev_id, bool hog)
{
	pid_t hog_pid;
	AudioObjectPropertyAddress aopa = {
		kAudioDevicePropertyHogMode,
		kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMaster
	};
	UInt32 size = sizeof(hog_pid);
	OSStatus err = AudioObjectGetPropertyData(dev_id,
						  &aopa,
						  0,
						  NULL,
						  &size,
						  &hog_pid);
	if (err != noErr) {
		d_print("Cannot get hog information: %d\n", err);
		return;
	}
	if (hog) {
		if (hog_pid != -1) {
			d_print("Device is already hogged.\n");
			return;
		}
	} else {
		if (hog_pid != getpid()) {
			d_print("Device is not owned by this process.\n");
			return;
		}
	}
	hog_pid = hog ? getpid() : -1;
	size = sizeof(hog_pid);
	err = AudioObjectSetPropertyData(dev_id,
					 &aopa,
					 0,
					 NULL,
					 size,
					 &hog_pid);
	if (err != noErr)
		d_print("Cannot hog the device: %d\n", err);
}

static OSStatus coreaudio_set_buffer_size(AudioUnit au, AudioStreamBasicDescription desc)
{
	AudioValueRange value_range = {0, 0};
	UInt32 property_size = sizeof(AudioValueRange);
	OSStatus err = AudioUnitGetProperty(au,
					    kAudioDevicePropertyBufferFrameSizeRange,
					    kAudioUnitScope_Global,
					    0,
					    &value_range,
					    &property_size);
	if (err != noErr)
		return err;

	UInt32 buffer_frame_size = value_range.mMaximum;
	err = AudioUnitSetProperty(au,
				   kAudioDevicePropertyBufferFrameSize,
				   kAudioUnitScope_Global,
				   0,
				   &buffer_frame_size,
				   sizeof(buffer_frame_size));
	if (err != noErr)
		d_print("Failed to set maximum buffer size: %d\n", err);

	return noErr;
}

static OSStatus coreaudio_init_audio_unit(AudioUnit *au,
					  OSType os_type,
					  AudioDeviceID dev_id)
{
	OSStatus err;
	AudioComponentDescription comp_desc = {
		kAudioUnitType_Output,
		os_type,
		kAudioUnitManufacturer_Apple,
		0,
		0
	};

	AudioComponent comp = AudioComponentFindNext(0, &comp_desc);
	if (!comp) {
		return -1;
	}

	err = AudioComponentInstanceNew(comp, au);
	if (err != noErr)
		return err;

	if (os_type == kAudioUnitSubType_HALOutput) {
		err = AudioUnitSetProperty(*au,
					   kAudioOutputUnitProperty_CurrentDevice,
					   kAudioUnitScope_Global,
					   0,
					   &dev_id,
					   sizeof(dev_id));
		if (err != noErr)
			return err;
	}

	return err;
}

static OSStatus coreaudio_start_audio_unit(AudioUnit *au,
					   AudioStreamBasicDescription desc)
{

	OSStatus err;
	err = AudioUnitSetProperty(*au,
				   kAudioUnitProperty_StreamFormat,
				   kAudioUnitScope_Input,
				   0,
				   &desc,
				   sizeof(desc));
	if (err != noErr)
		return err;

	AURenderCallbackStruct cb = {
		.inputProc = coreaudio_play_callback,
		.inputProcRefCon = NULL,
	};
	err = AudioUnitSetProperty(*au,
				   kAudioUnitProperty_SetRenderCallback,
				   kAudioUnitScope_Input,
				   0,
				   &cb,
				   sizeof(cb));
	if (err != noErr)
		return err;

	err = AudioUnitInitialize(*au);
	if (err != noErr)
		return err;

	err = coreaudio_set_buffer_size(*au, desc);
	if (err != noErr)
		return err;

	return AudioOutputUnitStart(*au);
}

static int coreaudio_init(void)
{
	AudioDeviceID default_dev_id = coreaudio_get_default_device();
	if (default_dev_id == kAudioDeviceUnknown) {
		errno = ENODEV;
		return -OP_ERROR_ERRNO;
	}

	AudioDeviceID named_dev_id = kAudioDeviceUnknown;
	if (coreaudio_opt_device_name)
		named_dev_id = coreaudio_find_device(coreaudio_opt_device_name);

	coreaudio_device_id = named_dev_id != kAudioDeviceUnknown ? named_dev_id : default_dev_id;

	if (named_dev_id != kAudioDeviceUnknown && coreaudio_opt_enable_hog_mode)
		coreaudio_hog_device(coreaudio_device_id, true);

	OSType unit_subtype = named_dev_id != kAudioDeviceUnknown ?
					kAudioUnitSubType_HALOutput :
					kAudioUnitSubType_DefaultOutput;
	OSStatus err = coreaudio_init_audio_unit(&coreaudio_audio_unit,
						 unit_subtype,
						 coreaudio_device_id);
	if (err != noErr) {
		errno = ENODEV;
		return -OP_ERROR_ERRNO;
	}
	return OP_ERROR_SUCCESS;
}

static int coreaudio_exit(void)
{
	AudioComponentInstanceDispose(coreaudio_audio_unit);
	coreaudio_audio_unit = NULL;
	coreaudio_hog_device(coreaudio_device_id, false);
	AudioHardwareUnload();
	coreaudio_device_id = kAudioDeviceUnknown;
	return OP_ERROR_SUCCESS;
}

static int coreaudio_open(sample_format_t sf, const channel_position_t *channel_map)
{

	coreaudio_format_description = coreaudio_fill_format_description(sf);
	if (coreaudio_opt_sync_rate)
		coreaudio_sync_device_sample_rate(coreaudio_device_id, coreaudio_format_description);
	if (channel_map)
		coreaudio_set_channel_position(coreaudio_device_id,
					       coreaudio_format_description.mChannelsPerFrame,
					       channel_map);
	pthread_mutex_init(&mutex, NULL);
	pthread_cond_init(&cond, NULL);
	OSStatus err = coreaudio_start_audio_unit(&coreaudio_audio_unit,
						  coreaudio_format_description);
	if (err)
		return -OP_ERROR_SAMPLE_FORMAT;
	return OP_ERROR_SUCCESS;
}

static void coreaudio_flush_buffer(bool drop) {
	bool locked;

	/* while (!blocking) // wait until a callback kicks in */
	/* 	; */
	/* while (coreaudio_buffer_size == 0) // wait until a callback kicks in */
	/* 	ms_sleep(25); // mimick the consumer loop */

	stopping = !drop; // after wait loop; synchronous

	locked = !pthread_mutex_lock(&mutex);

	if (partial) { // synchronous
		if (!drop)
			memset(coreaudio_buffer, 0, coreaudio_buffer_size);
		partial = false;
	}

	if (coreaudio_buffer_size !=0) {
		dropping = drop;  // asynchronous
		coreaudio_buffer_size = 0; // synchronous
	}
	/* do { */
		pthread_cond_signal(&cond); // shouldn't hurt if locking somehow failed
	/* } while (blocking);  // we need a callback to unset this; asynchronous */

	if (locked)
		pthread_mutex_unlock(&mutex);
}

static int coreaudio_close(void)
{
	coreaudio_flush_buffer(false);
	AudioOutputUnitStop(coreaudio_audio_unit);

	AudioUnitUninitialize(coreaudio_audio_unit);
	pthread_cond_destroy(&cond);
	pthread_mutex_destroy(&mutex);
	stopping = false;

	return OP_ERROR_SUCCESS;
}

static int coreaudio_drop(void)
{
	coreaudio_flush_buffer(true);
	return OP_ERROR_SUCCESS;
}

static int coreaudio_write(const char *buf, int cnt)
{
	struct timeval start;

	memcpy(coreaudio_buffer, buf, cnt);
	d_print("written to coreaudio: %d\n", cnt);
	coreaudio_buffer_size -= cnt;
	gettimeofday(&start, NULL);
	d_print("time: %ld\n", (long) start.tv_usec);
	if (coreaudio_buffer_size == 0) {
		pthread_mutex_lock(&mutex);
		pthread_cond_signal(&cond);
		pthread_mutex_unlock(&mutex);
		partial = false;
	} else {
		coreaudio_buffer += cnt;
		partial = true;
	}
	return cnt;
}

static OSStatus coreaudio_get_device_stereo_channels(AudioDeviceID dev_id, UInt32 *channels) {
	AudioObjectPropertyAddress aopa = {
		kAudioDevicePropertyPreferredChannelsForStereo,
		kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMaster
	};
	UInt32 size = sizeof(UInt32[2]);
	OSStatus err = AudioObjectGetPropertyData(dev_id,
						  &aopa,
						  0,
						  NULL,
						  &size,
						  channels);
	return err;
}

static int coreaudio_mixer_set_volume(int l, int r)
{
	Float32 vol[2];
	OSStatus err = 0;
	for (int i = 0; i < 2; i++) {
		vol[i]  = (i == 0 ? l : r) * 1.0f / coreaudio_max_volume;
		if (vol[i] > 1.0f)
			vol[i] = 1.0f;
		if (vol[i] < 0.0f)
			vol[i] = 0.0f;
		AudioObjectPropertyAddress aopa = {
			.mSelector	= kAudioDevicePropertyVolumeScalar,
			.mScope		= kAudioObjectPropertyScopeOutput,
			.mElement	= coreaudio_stereo_channels[i]
		};

		UInt32 size = sizeof(vol[i]);
		err |= AudioObjectSetPropertyData(coreaudio_device_id,
						  &aopa,
						  0,
						  NULL,
						  size,
						  vol + i);
	}
	if (err != noErr) {
		errno = ENODEV;
		return -OP_ERROR_ERRNO;
	}
	return OP_ERROR_SUCCESS;
}

static int coreaudio_mixer_get_volume(int *l, int *r)
{
	clear_pipe(coreaudio_mixer_pipe_out, -1);
	Float32 vol[2] = {.0, .0};
	OSStatus err = 0;
	for (int i = 0; i < 2; i++) {
		AudioObjectPropertyAddress aopa = {
			.mSelector	= kAudioDevicePropertyVolumeScalar,
			.mScope		= kAudioObjectPropertyScopeOutput,
			.mElement	= coreaudio_stereo_channels[i]
		};
		UInt32 size = sizeof(vol[i]);
		err |= AudioObjectGetPropertyData(coreaudio_device_id,
						  &aopa,
						  0,
						  NULL,
						  &size,
						  vol + i);
		int volume = vol[i] * coreaudio_max_volume;
		if (volume > coreaudio_max_volume)
			volume = coreaudio_max_volume;
		if (volume < 0)
			volume = 0;
		if (i == 0) {
			*l = volume;
		} else {
			*r = volume;
		}
	}
	if (err != noErr) {
		errno = ENODEV;
		return -OP_ERROR_ERRNO;
	}
	return OP_ERROR_SUCCESS;
}

static int coreaudio_mixer_open(int *volume_max)
{
	*volume_max = coreaudio_max_volume;
	OSStatus err = coreaudio_get_device_stereo_channels(coreaudio_device_id, coreaudio_stereo_channels);
	if (err != noErr) {
		d_print("Cannot get channel information: %d\n", err);
		errno = ENODEV;
		return -OP_ERROR_ERRNO;
	}
	for (int i = 0; i < 2; i++) {
		AudioObjectPropertyAddress aopa = {
			.mSelector	= kAudioDevicePropertyVolumeScalar,
			.mScope		= kAudioObjectPropertyScopeOutput,
			.mElement	= coreaudio_stereo_channels[i]
		};
		err |= AudioObjectAddPropertyListener(coreaudio_device_id,
						      &aopa,
						      coreaudio_device_volume_change_listener,
						      NULL);
	}
	if (err != noErr) {
		d_print("Cannot add property listener: %d\n", err);
		errno = ENODEV;
		return -OP_ERROR_ERRNO;
	}
	init_pipes(&coreaudio_mixer_pipe_out, &coreaudio_mixer_pipe_in);
	return OP_ERROR_SUCCESS;
}

static int coreaudio_mixer_close(void)
{
	OSStatus err = noErr;
	for (int i = 0; i < 2; i++) {
		AudioObjectPropertyAddress aopa = {
			.mSelector	= kAudioDevicePropertyVolumeScalar,
			.mScope		= kAudioObjectPropertyScopeOutput,
			.mElement	= coreaudio_stereo_channels[i]
		};

		err |= AudioObjectRemovePropertyListener(coreaudio_device_id,
							 &aopa,
							 coreaudio_device_volume_change_listener,
							 NULL);
	}
	if (err != noErr) {
		d_print("Cannot remove property listener: %d\n", err);
		errno = ENODEV;
		return -OP_ERROR_ERRNO;
	}
	close(coreaudio_mixer_pipe_out);
	close(coreaudio_mixer_pipe_in);
	return OP_ERROR_SUCCESS;
}

static int coreaudio_mixer_dummy(void)
{
	return OP_ERROR_SUCCESS;
}

static int coreaudio_mixer_get_fds(int *fds)
{
	fds[0] = coreaudio_mixer_pipe_out;
	return 1;
}

static int coreaudio_pause(void)
{
	coreaudio_flush_buffer(false);
	OSStatus err = AudioOutputUnitStop(coreaudio_audio_unit);
	if (err != noErr) {
		errno = ENODEV;
		return -OP_ERROR_ERRNO;
	}
	return OP_ERROR_SUCCESS;
}

static int coreaudio_unpause(void)
{
	stopping = false;
	OSStatus err = AudioOutputUnitStart(coreaudio_audio_unit);
	if (err != noErr) {
		errno = ENODEV;
		return -OP_ERROR_ERRNO;
	}
	return OP_ERROR_SUCCESS;
}

static int coreaudio_buffer_space(void)
{
	pthread_mutex_lock(&mutex);
	if (coreaudio_buffer_size == 0)
		// we can do timed wait here if timeout is ever useful
		pthread_cond_wait(&cond, &mutex);
	pthread_mutex_unlock(&mutex);
	return coreaudio_buffer_size;
}

static int coreaudio_buffer_space_delay(void)
{
	return 0;
}

static int coreaudio_set_sync_sample_rate(const char *val)
{
	coreaudio_opt_sync_rate = strcmp(val, "true") ? false : true;
	if (coreaudio_opt_sync_rate)
		coreaudio_sync_device_sample_rate(coreaudio_device_id, coreaudio_format_description);
	return 0;
}

static int coreaudio_get_sync_sample_rate(char **val)
{
	*val = xstrdup(coreaudio_opt_sync_rate ? "true" : "false");
	return 0;
}

static int coreaudio_set_enable_hog_mode(const char *val)
{
	coreaudio_opt_enable_hog_mode = strcmp(val, "true") ? false : true;
	coreaudio_hog_device(coreaudio_device_id, coreaudio_opt_enable_hog_mode);
	return 0;
}

static int coreaudio_get_enable_hog_mode(char **val)
{
	*val = xstrdup(coreaudio_opt_enable_hog_mode ? "true" : "false");
	return 0;
}

static int coreaudio_set_device(const char *val)
{
	free(coreaudio_opt_device_name);
	coreaudio_opt_device_name = NULL;
	if (val[0])
		coreaudio_opt_device_name = xstrdup(val);
	return 0;
}

static int coreaudio_get_device(char **val)
{
	if (coreaudio_opt_device_name)
		*val = xstrdup(coreaudio_opt_device_name);
	return 0;
}

const struct output_plugin_ops op_pcm_ops = {
	.init         = coreaudio_init,
	.exit         = coreaudio_exit,
	.open         = coreaudio_open,
	.close        = coreaudio_close,
	.drop         = coreaudio_drop,
	.write        = coreaudio_write,
	.pause        = coreaudio_pause,
	.unpause      = coreaudio_unpause,
	.buffer_space = coreaudio_buffer_space,
	.buffer_space_delay = coreaudio_buffer_space_delay,
};


const struct mixer_plugin_ops op_mixer_ops = {
	.init       = coreaudio_mixer_dummy,
	.exit       = coreaudio_mixer_dummy,
	.open       = coreaudio_mixer_open,
	.close      = coreaudio_mixer_close,
	.get_fds    = coreaudio_mixer_get_fds,
	.set_volume = coreaudio_mixer_set_volume,
	.get_volume = coreaudio_mixer_get_volume,
};

const struct output_plugin_opt op_pcm_options[] = {
	OPT(coreaudio, device),
	OPT(coreaudio, enable_hog_mode),
	OPT(coreaudio, sync_sample_rate),
	{ NULL },
};

const struct mixer_plugin_opt op_mixer_options[] = {
	{ NULL },
};

const int op_priority = 1;
const unsigned op_abi_version = OP_ABI_VERSION;
