/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* for ppoll */
#endif

#include <pthread.h>
#include <poll.h>
#include <stdbool.h>
#include <sys/param.h>
#include <syslog.h>

#include "audio_thread_log.h"
#include "cras_audio_thread_monitor.h"
#include "cras_config.h"
#include "cras_fmt_conv.h"
#include "cras_iodev.h"
#include "cras_rstream.h"
#include "cras_system_state.h"
#include "cras_types.h"
#include "cras_util.h"
#include "dev_stream.h"
#include "audio_thread.h"
#include "utlist.h"

#define MIN_PROCESS_TIME_US 500 /* 0.5ms - min amount of time to mix/src. */
#define SLEEP_FUZZ_FRAMES 10 /* # to consider "close enough" to sleep frames. */
#define MIN_READ_WAIT_US 2000 /* 2ms */
/*
 * # to check whether a busyloop event happens
 */
#define MAX_CONTINUOUS_ZERO_SLEEP_COUNT 2

/* Messages that can be sent from the main context to the audio thread. */
enum AUDIO_THREAD_COMMAND {
	AUDIO_THREAD_ADD_OPEN_DEV,
	AUDIO_THREAD_RM_OPEN_DEV,
	AUDIO_THREAD_IS_DEV_OPEN,
	AUDIO_THREAD_ADD_STREAM,
	AUDIO_THREAD_DISCONNECT_STREAM,
	AUDIO_THREAD_STOP,
	AUDIO_THREAD_DUMP_THREAD_INFO,
	AUDIO_THREAD_DRAIN_STREAM,
	AUDIO_THREAD_CONFIG_GLOBAL_REMIX,
	AUDIO_THREAD_DEV_START_RAMP,
	AUDIO_THREAD_REMOVE_CALLBACK,
	AUDIO_THREAD_AEC_DUMP,
};

struct audio_thread_msg {
	size_t length;
	enum AUDIO_THREAD_COMMAND id;
};

struct audio_thread_config_global_remix {
	struct audio_thread_msg header;
	struct cras_fmt_conv *fmt_conv;
};

struct audio_thread_open_device_msg {
	struct audio_thread_msg header;
	struct cras_iodev *dev;
};

struct audio_thread_rm_callback_msg {
	struct audio_thread_msg header;
	int fd;
};

struct audio_thread_add_rm_stream_msg {
	struct audio_thread_msg header;
	struct cras_rstream *stream;
	struct cras_iodev **devs;
	unsigned int num_devs;
};

struct audio_thread_dump_debug_info_msg {
	struct audio_thread_msg header;
	struct audio_debug_info *info;
};

struct audio_thread_dev_start_ramp_msg {
	struct audio_thread_msg header;
	struct cras_iodev *dev;
	enum CRAS_IODEV_RAMP_REQUEST request;
};

struct audio_thread_aec_dump_msg {
	struct audio_thread_msg header;
	cras_stream_id_t stream_id;
	unsigned int start; /* */
	int fd;
};

/* Audio thread logging. */
struct audio_thread_event_log *atlog;

static struct iodev_callback_list *iodev_callbacks;
static struct timespec longest_wake;

struct iodev_callback_list {
	int fd;
	int is_write;
	int enabled;
	thread_callback cb;
	void *cb_data;
	struct pollfd *pollfd;
	struct iodev_callback_list *prev, *next;
};

static void _audio_thread_add_callback(int fd, thread_callback cb,
				       void *data, int is_write)
{
	struct iodev_callback_list *iodev_cb;

	/* Don't add iodev_cb twice */
	DL_FOREACH(iodev_callbacks, iodev_cb)
		if (iodev_cb->fd == fd && iodev_cb->cb_data == data)
			return;

	iodev_cb = (struct iodev_callback_list *)calloc(1, sizeof(*iodev_cb));
	iodev_cb->fd = fd;
	iodev_cb->cb = cb;
	iodev_cb->cb_data = data;
	iodev_cb->enabled = 1;
	iodev_cb->is_write = is_write;

	DL_APPEND(iodev_callbacks, iodev_cb);
}

void audio_thread_add_callback(int fd, thread_callback cb,
				void *data)
{
	_audio_thread_add_callback(fd, cb, data, 0);
}

void audio_thread_add_write_callback(int fd, thread_callback cb,
				     void *data)
{
	_audio_thread_add_callback(fd, cb, data, 1);
}

void audio_thread_rm_callback(int fd)
{
	struct iodev_callback_list *iodev_cb;

	DL_FOREACH(iodev_callbacks, iodev_cb) {
		if (iodev_cb->fd == fd) {
			DL_DELETE(iodev_callbacks, iodev_cb);
			free(iodev_cb);
			return;
		}
	}
}

void audio_thread_enable_callback(int fd, int enabled)
{
	struct iodev_callback_list *iodev_cb;

	DL_FOREACH(iodev_callbacks, iodev_cb) {
		if (iodev_cb->fd == fd) {
			iodev_cb->enabled = !!enabled;
			return;
		}
	}
}

/* Sends a response (error code) from the audio thread to the main thread.
 * Indicates that the last message sent to the audio thread has been handled
 * with an error code of rc.
 * Args:
 *    thread - thread responding to command.
 *    rc - Result code to send back to the main thread.
 * Returns:
 *    The number of bytes written to the main thread.
 */
static int audio_thread_send_response(struct audio_thread *thread, int rc)
{
	return write(thread->to_main_fds[1], &rc, sizeof(rc));
}

/* Reads from a file descriptor until all bytes are read.
 *
 * Args:
 *    fd - file descriptor to read
 *    buf - the buffer to be written.
 *    count - the number of bytes to read from fd
 * Returns:
 *    |count| on success, negative error code on failure.
 */
static int read_until_finished(int fd, void *buf, size_t count) {
	int nread, count_left = count;

	while (count_left > 0) {
		nread = read(fd, (uint8_t *)buf + count - count_left,
			     count_left);
		if (nread < 0) {
			if (errno == EINTR)
				continue;
			else
				return nread;
		} else if (nread == 0) {
			syslog(LOG_ERR, "Pipe has been closed.");
			return -EPIPE;
		}
		count_left -= nread;
	}
	return count;
}

/* Reads a command from the main thread.  Called from the playback/capture
 * thread.  This will read the next available command from the main thread and
 * put it in buf.
 * Args:
 *    thread - thread reading the command.
 *    buf - Message is stored here on return.
 *    max_len - maximum length of message to put into buf.
 * Returns:
 *    0 on success, negative error code on failure.
 */
static int audio_thread_read_command(struct audio_thread *thread,
				     uint8_t *buf,
				     size_t max_len)
{
	int to_read, nread, rc;
	struct audio_thread_msg *msg = (struct audio_thread_msg *)buf;

	/* Get the length of the message first */
	nread = read_until_finished(
			thread->to_thread_fds[0], buf, sizeof(msg->length));
	if (nread < 0)
		return nread;

	if (msg->length > max_len)
		return -ENOMEM;

	to_read = msg->length - sizeof(msg->length);
	rc = read_until_finished(thread->to_thread_fds[0],
			&buf[0] + sizeof(msg->length), to_read);
	if (rc < 0)
		return rc;
	return 0;
}

/* Builds an initial buffer to avoid an underrun. Adds min_level of latency. */
static void fill_odevs_zeros_min_level(struct cras_iodev *odev)
{
	cras_iodev_fill_odev_zeros(odev, odev->min_buffer_level);
}

/* Append a new stream to a specified set of iodevs. */
static int append_stream(struct audio_thread *thread,
			 struct cras_rstream *stream,
			 struct cras_iodev **iodevs,
			 unsigned int num_iodevs)
{
	struct open_dev *open_dev;
	struct cras_iodev *dev;
	struct dev_stream *out;
	struct timespec init_cb_ts;
	const struct timespec *stream_ts;
	unsigned int i;
	bool cb_ts_set = false;
	int rc = 0;

	for (i = 0; i < num_iodevs; i++) {
		DL_SEARCH_SCALAR(thread->open_devs[stream->direction], open_dev,
				dev, iodevs[i]);
		if (!open_dev)
			continue;

		dev = iodevs[i];
		DL_SEARCH_SCALAR(dev->streams, out, stream, stream);
		if (out)
			continue;

		/* For output, if open device already has stream, get the earliest next
		 * callback time from these streams to align with. Otherwise, use the
		 * timestamp now as the initial callback time for new stream so dev_stream
		 * can set its own schedule.
		 * If next callback time is too far from now, it will block writing and
		 * lower hardware level. Else if we fetch the new stream immediately, it
		 * may cause device buffer level stack up.
		 */
		if (stream->direction == CRAS_STREAM_OUTPUT && dev->streams) {
			DL_FOREACH(dev->streams, out) {
				stream_ts =  dev_stream_next_cb_ts(out);
				if (stream_ts &&
				    (!cb_ts_set || timespec_after(&init_cb_ts, stream_ts))) {
					init_cb_ts = *stream_ts;
					cb_ts_set = true;
				}
			}
		}

		if (!cb_ts_set)
			clock_gettime(CLOCK_MONOTONIC_RAW, &init_cb_ts);

		out = dev_stream_create(stream, dev->info.idx,
					dev->ext_format, dev, &init_cb_ts);
		if (!out) {
			rc = -EINVAL;
			break;
		}

		/* When the first input stream is added, flush the input buffer
		 * so that we can read from multiple input devices of the same
		 * buffer level.
		 */
		if ((stream->direction == CRAS_STREAM_INPUT) && !dev->streams) {
			int num_flushed = dev->flush_buffer(dev);
			if (num_flushed < 0) {
				rc = num_flushed;
				break;
			}
		}

		cras_iodev_add_stream(dev, out);

		/* For multiple inputs case, if the new stream is not the first
		 * one to append, copy the 1st stream's offset to it so that
		 * future read offsets can be aligned across all input streams
		 * to avoid the deadlock scenario when multiple streams reading
		 * from multiple devices.
		 */
		if ((stream->direction == CRAS_STREAM_INPUT) &&
		    (dev->streams != out)) {
			unsigned int offset =
				cras_iodev_stream_offset(dev, dev->streams);
			if (offset > stream->cb_threshold)
				offset = stream->cb_threshold;
			cras_iodev_stream_written(dev, out, offset);

			offset = cras_rstream_dev_offset(dev->streams->stream,
							 dev->info.idx);
			if (offset > stream->cb_threshold)
				offset = stream->cb_threshold;
			cras_rstream_dev_offset_update(stream, offset,
						       dev->info.idx);
		}
	}

	if (rc) {
		DL_FOREACH(thread->open_devs[stream->direction], open_dev) {
			dev = open_dev->dev;
			DL_SEARCH_SCALAR(dev->streams, out, stream, stream);
			if (!out)
				continue;

			cras_iodev_rm_stream(dev, stream);
			dev_stream_destroy(out);
		}
	}

	return rc;
}

/* Handles messages from main thread to add a new active device. */
static int thread_add_open_dev(struct audio_thread *thread,
			       struct cras_iodev *iodev)
{
	struct open_dev *adev;

	DL_SEARCH_SCALAR(thread->open_devs[iodev->direction],
			 adev, dev, iodev);
	if (adev)
		return -EEXIST;

	adev = (struct open_dev *)calloc(1, sizeof(*adev));
	adev->dev = iodev;

	/*
	 * Start output devices by padding the output. This avoids a burst of
	 * audio callbacks when the stream starts
	 */
	if (iodev->direction == CRAS_STREAM_OUTPUT)
		fill_odevs_zeros_min_level(iodev);

	ATLOG(atlog, AUDIO_THREAD_DEV_ADDED, iodev->info.idx, 0, 0);

	DL_APPEND(thread->open_devs[iodev->direction], adev);

	return 0;
}

/* Handles messages from the main thread to remove an active device. */
static int thread_rm_open_dev(struct audio_thread *thread,
			      struct cras_iodev *iodev)
{
	struct open_dev *adev = dev_io_find_open_dev(
			thread->open_devs[iodev->direction], iodev);
	if (!adev)
		return -EINVAL;

	dev_io_rm_open_dev(&thread->open_devs[iodev->direction], adev);
	return 0;
}

/*
 * Handles message from the main thread to check if an iodev is in the
 * open dev list.
 */
static int thread_is_dev_open(struct audio_thread *thread,
			      struct cras_iodev *iodev)
{
	struct open_dev *adev = dev_io_find_open_dev(
			thread->open_devs[iodev->direction], iodev);
	return !!adev;
}

/* Handles messages from the main thread to start ramping on a device. */
static int thread_dev_start_ramp(struct audio_thread *thread,
				 struct cras_iodev *iodev,
				 enum CRAS_IODEV_RAMP_REQUEST request)
{
	/* Do nothing if device wasn't already in the active dev list. */
	struct open_dev *adev = dev_io_find_open_dev(
			thread->open_devs[iodev->direction], iodev);
	if (!adev)
		return -EINVAL;
	return cras_iodev_start_ramp(iodev, request);
}


/* Return non-zero if the stream is attached to any device. */
static int thread_find_stream(struct audio_thread *thread,
			      struct cras_rstream *rstream)
{
	struct open_dev *open_dev;
	struct dev_stream *s;

	DL_FOREACH(thread->open_devs[rstream->direction], open_dev) {
		DL_FOREACH(open_dev->dev->streams, s) {
			if (s->stream == rstream)
				return 1;
		}
	}
	return 0;
}

/* Handles the disconnect_stream message from the main thread. */
static int thread_disconnect_stream(struct audio_thread* thread,
				    struct cras_rstream* stream,
				    struct cras_iodev *dev)
{
	int rc;

	if (!thread_find_stream(thread, stream))
		return 0;

	rc = dev_io_remove_stream(&thread->open_devs[stream->direction],
				  stream, dev);

	return rc;
}

/* Initiates draining of a stream or returns the status of a draining stream.
 * If the stream has completed draining the thread forfeits ownership and must
 * never reference it again.  Returns the number of milliseconds it will take to
 * finish draining, a minimum of one ms if any samples remain.
 */
static int thread_drain_stream_ms_remaining(struct audio_thread *thread,
					    struct cras_rstream *rstream)
{
	int fr_in_buff;
	struct cras_audio_shm *shm;

	if (rstream->direction != CRAS_STREAM_OUTPUT)
		return 0;

	shm = cras_rstream_output_shm(rstream);
	fr_in_buff = cras_shm_get_frames(shm);

	if (fr_in_buff <= 0)
		return 0;

	cras_rstream_set_is_draining(rstream, 1);

	return 1 + cras_frames_to_ms(fr_in_buff, rstream->format.frame_rate);
}

/* Handles a request to begin draining and return the amount of time left to
 * draing a stream.
 */
static int thread_drain_stream(struct audio_thread *thread,
			       struct cras_rstream *rstream)
{
	int ms_left;

	if (!thread_find_stream(thread, rstream))
		return 0;

	ms_left = thread_drain_stream_ms_remaining(thread, rstream);
	if (ms_left == 0)
		dev_io_remove_stream(&thread->open_devs[rstream->direction],
				     rstream, NULL);

	return ms_left;
}

/* Handles the add_stream message from the main thread. */
static int thread_add_stream(struct audio_thread *thread,
			     struct cras_rstream *stream,
			     struct cras_iodev **iodevs,
			     unsigned int num_iodevs)
{
	int rc;

	rc = append_stream(thread, stream, iodevs, num_iodevs);
	if (rc < 0)
		return rc;

	ATLOG(atlog, AUDIO_THREAD_STREAM_ADDED, stream->stream_id,
	      num_iodevs ? iodevs[0]->info.idx : 0, num_iodevs);
	return 0;
}

/* Starts or stops aec dump task. */
static int thread_set_aec_dump(struct audio_thread *thread,
			       cras_stream_id_t stream_id,
			       unsigned int start,
			       int fd)
{
	struct open_dev *idev_list = thread->open_devs[CRAS_STREAM_INPUT];
	struct open_dev *adev;
	struct dev_stream *stream;

	DL_FOREACH(idev_list, adev) {
		if (!cras_iodev_is_open(adev->dev))
			continue;

		DL_FOREACH(adev->dev->streams, stream) {
			if ((stream->stream->apm_list == NULL) ||
			    (stream->stream->stream_id != stream_id))
				continue;

			cras_apm_list_set_aec_dump(stream->stream->apm_list,
						   adev->dev, start, fd);
		}
	}
	return 0;
}

/* Stop the playback thread */
static void terminate_pb_thread()
{
	pthread_exit(0);
}

static void append_dev_dump_info(struct audio_dev_debug_info *di,
				 struct open_dev *adev)
{
	struct cras_audio_format *fmt = adev->dev->ext_format;
	strncpy(di->dev_name, adev->dev->info.name, sizeof(di->dev_name));
	di->buffer_size = adev->dev->buffer_size;
	di->min_buffer_level = adev->dev->min_buffer_level;
	di->min_cb_level = adev->dev->min_cb_level;
	di->max_cb_level = adev->dev->max_cb_level;
	di->direction = adev->dev->direction;
	di->num_underruns = cras_iodev_get_num_underruns(adev->dev);
	di->num_severe_underruns = cras_iodev_get_num_severe_underruns(
			adev->dev);
	di->highest_hw_level = adev->dev->highest_hw_level;
	if (fmt) {
		di->frame_rate = fmt->frame_rate;
		di->num_channels = fmt->num_channels;
		di->est_rate_ratio = cras_iodev_get_est_rate_ratio(adev->dev);
	} else {
		di->frame_rate = 0;
		di->num_channels = 0;
		di->est_rate_ratio = 0;
	}
}

/* Put stream info for the given stream into the info struct. */
static void append_stream_dump_info(struct audio_debug_info *info,
				    struct dev_stream *stream,
				    unsigned int dev_idx,
				    int index)
{
	struct audio_stream_debug_info *si;

	si = &info->streams[index];

	si->stream_id = stream->stream->stream_id;
	si->dev_idx = dev_idx;
	si->direction = stream->stream->direction;
	si->stream_type = stream->stream->stream_type;
	si->buffer_frames = stream->stream->buffer_frames;
	si->cb_threshold = stream->stream->cb_threshold;
	si->frame_rate = stream->stream->format.frame_rate;
	si->num_channels = stream->stream->format.num_channels;
	memcpy(si->channel_layout, stream->stream->format.channel_layout,
	       sizeof(si->channel_layout));
	si->longest_fetch_sec = stream->stream->longest_fetch_interval.tv_sec;
	si->longest_fetch_nsec = stream->stream->longest_fetch_interval.tv_nsec;
	si->num_overruns = cras_shm_num_overruns(&stream->stream->shm);
	si->effects = cras_apm_list_get_effects(stream->stream->apm_list);

	longest_wake.tv_sec = 0;
	longest_wake.tv_nsec = 0;
}

/* Handle a message sent to the playback thread */
static int handle_playback_thread_message(struct audio_thread *thread)
{
	uint8_t buf[256];
	struct audio_thread_msg *msg = (struct audio_thread_msg *)buf;
	int ret = 0;
	int err;

	err = audio_thread_read_command(thread, buf, 256);
	if (err < 0)
		return err;

	ATLOG(atlog, AUDIO_THREAD_PB_MSG, msg->id, 0, 0);

	switch (msg->id) {
	case AUDIO_THREAD_ADD_STREAM: {
		struct audio_thread_add_rm_stream_msg *amsg;
		amsg = (struct audio_thread_add_rm_stream_msg *)msg;
		ATLOG(atlog, AUDIO_THREAD_WRITE_STREAMS_WAIT,
		      amsg->stream->stream_id, 0, 0);
		ret = thread_add_stream(thread, amsg->stream, amsg->devs,
				amsg->num_devs);
		break;
	}
	case AUDIO_THREAD_DISCONNECT_STREAM: {
		struct audio_thread_add_rm_stream_msg *rmsg;

		rmsg = (struct audio_thread_add_rm_stream_msg *)msg;

		ret = thread_disconnect_stream(thread, rmsg->stream,
				rmsg->devs[0]);
		break;
	}
	case AUDIO_THREAD_ADD_OPEN_DEV: {
		struct audio_thread_open_device_msg *rmsg;

		rmsg = (struct audio_thread_open_device_msg *)msg;
		ret = thread_add_open_dev(thread, rmsg->dev);
		break;
	}
	case AUDIO_THREAD_RM_OPEN_DEV: {
		struct audio_thread_open_device_msg *rmsg;

		rmsg = (struct audio_thread_open_device_msg *)msg;
		ret = thread_rm_open_dev(thread, rmsg->dev);
		break;
	}
	case AUDIO_THREAD_IS_DEV_OPEN: {
		struct audio_thread_open_device_msg *rmsg;

		rmsg = (struct audio_thread_open_device_msg *)msg;
		ret = thread_is_dev_open(thread, rmsg->dev);
		break;
	}
	case AUDIO_THREAD_STOP:
		ret = 0;
		err = audio_thread_send_response(thread, ret);
		if (err < 0)
			return err;
		terminate_pb_thread();
		break;
	case AUDIO_THREAD_DUMP_THREAD_INFO: {
		struct dev_stream *curr;
		struct open_dev *adev;
		struct audio_thread_dump_debug_info_msg *dmsg;
		struct audio_debug_info *info;
		unsigned int num_streams = 0;
		unsigned int num_devs = 0;

		ret = 0;
		dmsg = (struct audio_thread_dump_debug_info_msg *)msg;
		info = dmsg->info;

		/* Go through all open devices. */
		DL_FOREACH(thread->open_devs[CRAS_STREAM_OUTPUT], adev) {
			append_dev_dump_info(&info->devs[num_devs], adev);
			if (++num_devs == MAX_DEBUG_DEVS)
				break;
			DL_FOREACH(adev->dev->streams, curr) {
				if (num_streams == MAX_DEBUG_STREAMS)
					break;
				append_stream_dump_info(info, curr,
							adev->dev->info.idx,
							num_streams++);
			}
		}
		DL_FOREACH(thread->open_devs[CRAS_STREAM_INPUT], adev) {
			if (num_devs == MAX_DEBUG_DEVS)
				break;
			append_dev_dump_info(&info->devs[num_devs], adev);
			DL_FOREACH(adev->dev->streams, curr) {
				if (num_streams == MAX_DEBUG_STREAMS)
					break;
				append_stream_dump_info(info, curr,
							adev->dev->info.idx,
							num_streams++);
			}
			++num_devs;
		}
		info->num_devs = num_devs;

		info->num_streams = num_streams;

		memcpy(&info->log, atlog, sizeof(info->log));
		break;
	}
	case AUDIO_THREAD_DRAIN_STREAM: {
		struct audio_thread_add_rm_stream_msg *rmsg;

		rmsg = (struct audio_thread_add_rm_stream_msg *)msg;
		ret = thread_drain_stream(thread, rmsg->stream);
		break;
	}
	case AUDIO_THREAD_REMOVE_CALLBACK: {
		struct audio_thread_rm_callback_msg *rmsg;

		rmsg = (struct audio_thread_rm_callback_msg *)msg;
		audio_thread_rm_callback(rmsg->fd);
		break;
	}
	case AUDIO_THREAD_CONFIG_GLOBAL_REMIX: {
		struct audio_thread_config_global_remix *rmsg;
		void *rsp;

		/* Respond the pointer to the old remix converter, so it can be
		 * freed later in main thread. */
		rsp = (void *)thread->remix_converter;

		rmsg = (struct audio_thread_config_global_remix *)msg;
		thread->remix_converter = rmsg->fmt_conv;

		return write(thread->to_main_fds[1], &rsp, sizeof(rsp));
	}
	case AUDIO_THREAD_DEV_START_RAMP: {
		struct audio_thread_dev_start_ramp_msg *rmsg;

		rmsg = (struct audio_thread_dev_start_ramp_msg*)msg;
		ret = thread_dev_start_ramp(thread, rmsg->dev, rmsg->request);
		break;
	}
	case AUDIO_THREAD_AEC_DUMP: {
		struct audio_thread_aec_dump_msg *rmsg;
		rmsg = (struct audio_thread_aec_dump_msg *)msg;
		ret = thread_set_aec_dump(thread, rmsg->stream_id,
					  rmsg->start, rmsg->fd);
		break;
	}
	default:
		ret = -EINVAL;
		break;
	}

	err = audio_thread_send_response(thread, ret);
	if (err < 0)
		return err;
	return ret;
}

/* Fills the time that the next stream needs to be serviced. */
static int get_next_stream_wake_from_list(struct dev_stream *streams,
					  struct timespec *min_ts)
{
	struct dev_stream *dev_stream;
	int ret = 0; /* The total number of streams to wait on. */

	DL_FOREACH(streams, dev_stream) {
		const struct timespec *next_cb_ts;

		if (cras_rstream_get_is_draining(dev_stream->stream) &&
		    dev_stream_playback_frames(dev_stream) <= 0)
			continue;
		if (!dev_stream_can_fetch(dev_stream))
			continue;

		next_cb_ts = dev_stream_next_cb_ts(dev_stream);
		if (!next_cb_ts)
			continue;

		ATLOG(atlog, AUDIO_THREAD_STREAM_SLEEP_TIME,
		      dev_stream->stream->stream_id, next_cb_ts->tv_sec,
		      next_cb_ts->tv_nsec);
		if (timespec_after(min_ts, next_cb_ts))
			*min_ts = *next_cb_ts;
		ret++;
	}

	return ret;
}

static int get_next_output_wake(struct open_dev **odevs,
				struct timespec *min_ts,
				const struct timespec *now)
{
	struct open_dev *adev;
	int ret = 0;

	DL_FOREACH(*odevs, adev)
		ret += get_next_stream_wake_from_list(
				adev->dev->streams,
				min_ts);

	DL_FOREACH(*odevs, adev) {
		if (!cras_iodev_odev_should_wake(adev->dev))
			continue;

		ret++;
		if (timespec_after(min_ts, &adev->wake_ts))
			*min_ts = adev->wake_ts;
	}

	return ret;
}

/* Returns the number of active streams plus the number of active devices. */
static int fill_next_sleep_interval(struct audio_thread *thread,
				    struct timespec *ts)
{
	struct timespec min_ts;
	struct timespec now;
	int ret;

	ts->tv_sec = 0;
	ts->tv_nsec = 0;
	/* Limit the sleep time to 20 seconds. */
	min_ts.tv_sec = 20;
	min_ts.tv_nsec = 0;
	clock_gettime(CLOCK_MONOTONIC_RAW, &now);
	add_timespecs(&min_ts, &now);
	ret = get_next_output_wake(&thread->open_devs[CRAS_STREAM_OUTPUT],
				   &min_ts, &now);
	ret += dev_io_next_input_wake(&thread->open_devs[CRAS_STREAM_INPUT],
				      &min_ts);
	if (timespec_after(&min_ts, &now))
		subtract_timespecs(&min_ts, &now, ts);

	return ret;
}

static struct pollfd *add_pollfd(struct audio_thread *thread,
				 int fd, int is_write)
{
	thread->pollfds[thread->num_pollfds].fd = fd;
	if (is_write)
		thread->pollfds[thread->num_pollfds].events = POLLOUT;
	else
		thread->pollfds[thread->num_pollfds].events = POLLIN;
	thread->num_pollfds++;
	if (thread->num_pollfds >= thread->pollfds_size) {
		thread->pollfds_size *= 2;
		thread->pollfds =
			(struct pollfd *)realloc(thread->pollfds,
						 sizeof(*thread->pollfds) *
						 thread->pollfds_size);
		return NULL;
	}

	return &thread->pollfds[thread->num_pollfds - 1];
}

static int continuous_zero_sleep_count = 0;
static void check_busyloop(struct timespec* wait_ts)
{
	if(wait_ts->tv_sec == 0 && wait_ts->tv_nsec == 0)
	{
		continuous_zero_sleep_count ++;
		if(continuous_zero_sleep_count ==
		   MAX_CONTINUOUS_ZERO_SLEEP_COUNT)
			cras_audio_thread_busyloop();
	}
	else
	{
		continuous_zero_sleep_count = 0;
	}
}

/* For playback, fill the audio buffer when needed, for capture, pull out
 * samples when they are ready.
 * This thread will attempt to run at a high priority to allow for low latency
 * streams.  This thread sleeps while the device plays back or captures audio,
 * it will wake up as little as it can while avoiding xruns.  It can also be
 * woken by sending it a message using the "audio_thread_post_message" function.
 */
static void *audio_io_thread(void *arg)
{
	struct audio_thread *thread = (struct audio_thread *)arg;
	struct open_dev *adev;
	struct dev_stream *curr;
	struct timespec ts, now, last_wake;
	int msg_fd;
	int rc;

	msg_fd = thread->to_thread_fds[0];

	/* Attempt to get realtime scheduling */
	if (cras_set_rt_scheduling(CRAS_SERVER_RT_THREAD_PRIORITY) == 0)
		cras_set_thread_priority(CRAS_SERVER_RT_THREAD_PRIORITY);

	last_wake.tv_sec = 0;
	longest_wake.tv_sec = 0;
	longest_wake.tv_nsec = 0;

	thread->pollfds[0].fd = msg_fd;
	thread->pollfds[0].events = POLLIN;

	while (1) {
		struct timespec *wait_ts;
		struct iodev_callback_list *iodev_cb;

		wait_ts = NULL;
		thread->num_pollfds = 1;

		/* device opened */
		dev_io_run(&thread->open_devs[CRAS_STREAM_OUTPUT],
			   &thread->open_devs[CRAS_STREAM_INPUT],
			   thread->remix_converter);

		if (fill_next_sleep_interval(thread, &ts))
			wait_ts = &ts;

restart_poll_loop:
		thread->num_pollfds = 1;

		DL_FOREACH(iodev_callbacks, iodev_cb) {
			if (!iodev_cb->enabled)
				continue;
			iodev_cb->pollfd = add_pollfd(thread, iodev_cb->fd,
						      iodev_cb->is_write);
			if (!iodev_cb->pollfd)
			    goto restart_poll_loop;
		}

		/* TODO(dgreid) - once per rstream not per dev_stream */
		DL_FOREACH(thread->open_devs[CRAS_STREAM_OUTPUT], adev) {
			DL_FOREACH(adev->dev->streams, curr) {
				int fd = dev_stream_poll_stream_fd(curr);
				if (fd < 0)
					continue;
				if (!add_pollfd(thread, fd, 0))
					goto restart_poll_loop;
			}
		}
		DL_FOREACH(thread->open_devs[CRAS_STREAM_INPUT], adev) {
			DL_FOREACH(adev->dev->streams, curr) {
				int fd = dev_stream_poll_stream_fd(curr);
				if (fd < 0)
					continue;
				if (!add_pollfd(thread, fd, 0))
					goto restart_poll_loop;
			}
		}

		if (last_wake.tv_sec) {
			struct timespec this_wake;
			clock_gettime(CLOCK_MONOTONIC_RAW, &now);
			subtract_timespecs(&now, &last_wake, &this_wake);
			if (timespec_after(&this_wake, &longest_wake))
				longest_wake = this_wake;
		}

		ATLOG(atlog, AUDIO_THREAD_SLEEP, wait_ts ? wait_ts->tv_sec : 0,
		      wait_ts ? wait_ts->tv_nsec : 0, longest_wake.tv_nsec);
		if(wait_ts)
			check_busyloop(wait_ts);
		rc = ppoll(thread->pollfds, thread->num_pollfds, wait_ts, NULL);
		clock_gettime(CLOCK_MONOTONIC_RAW, &last_wake);
		ATLOG(atlog, AUDIO_THREAD_WAKE, rc, 0, 0);
		if (rc <= 0)
			continue;

		if (thread->pollfds[0].revents & POLLIN) {
			rc = handle_playback_thread_message(thread);
			if (rc < 0)
				syslog(LOG_INFO, "handle message %d", rc);
		}

		DL_FOREACH(iodev_callbacks, iodev_cb) {
			if (iodev_cb->pollfd &&
			    iodev_cb->pollfd->revents & (POLLIN | POLLOUT)) {
				ATLOG(atlog, AUDIO_THREAD_IODEV_CB,
				      iodev_cb->is_write, 0, 0);
				iodev_cb->cb(iodev_cb->cb_data);
			}
		}
	}

	return NULL;
}

/* Write a message to the playback thread and wait for an ack, This keeps these
 * operations synchronous for the main server thread.  For instance when the
 * RM_STREAM message is sent, the stream can be deleted after the function
 * returns.  Making this synchronous also allows the thread to return an error
 * code that can be handled by the caller.
 * Args:
 *    thread - thread to receive message.
 *    msg - The message to send.
 * Returns:
 *    A return code from the message handler in the thread.
 */
static int audio_thread_post_message(struct audio_thread *thread,
				     struct audio_thread_msg *msg)
{
	int err, rsp;

	err = write(thread->to_thread_fds[1], msg, msg->length);
	if (err < 0) {
		syslog(LOG_ERR, "Failed to post message to thread.");
		return err;
	}
	/* Synchronous action, wait for response. */
	err = read_until_finished(thread->to_main_fds[0], &rsp, sizeof(rsp));
	if (err < 0) {
		syslog(LOG_ERR, "Failed to read reply from thread.");
		return err;
	}

	return rsp;
}

static void init_open_device_msg(struct audio_thread_open_device_msg *msg,
				 enum AUDIO_THREAD_COMMAND id,
				 struct cras_iodev *dev)
{
	memset(msg, 0, sizeof(*msg));
	msg->header.id = id;
	msg->header.length = sizeof(*msg);
	msg->dev = dev;
}

static void init_add_rm_stream_msg(struct audio_thread_add_rm_stream_msg *msg,
				   enum AUDIO_THREAD_COMMAND id,
				   struct cras_rstream *stream,
				   struct cras_iodev **devs,
				   unsigned int num_devs)
{
	memset(msg, 0, sizeof(*msg));
	msg->header.id = id;
	msg->header.length = sizeof(*msg);
	msg->stream = stream;
	msg->devs = devs;
	msg->num_devs = num_devs;
}

static void init_dump_debug_info_msg(
		struct audio_thread_dump_debug_info_msg *msg,
		struct audio_debug_info *info)
{
	memset(msg, 0, sizeof(*msg));
	msg->header.id = AUDIO_THREAD_DUMP_THREAD_INFO;
	msg->header.length = sizeof(*msg);
	msg->info = info;
}

static void init_config_global_remix_msg(
		struct audio_thread_config_global_remix *msg)
{
	memset(msg, 0, sizeof(*msg));
	msg->header.id = AUDIO_THREAD_CONFIG_GLOBAL_REMIX;
	msg->header.length = sizeof(*msg);
}

static void init_device_start_ramp_msg(
		struct audio_thread_dev_start_ramp_msg *msg,
		enum AUDIO_THREAD_COMMAND id,
		struct cras_iodev *dev,
		enum CRAS_IODEV_RAMP_REQUEST request)
{
	memset(msg, 0, sizeof(*msg));
	msg->header.id = id;
	msg->header.length = sizeof(*msg);
	msg->dev = dev;
	msg->request = request;
}

/* Exported Interface */

int audio_thread_add_stream(struct audio_thread *thread,
			    struct cras_rstream *stream,
			    struct cras_iodev **devs,
			    unsigned int num_devs)
{
	struct audio_thread_add_rm_stream_msg msg;

	assert(thread && stream);

	if (!thread->started)
		return -EINVAL;

	init_add_rm_stream_msg(&msg, AUDIO_THREAD_ADD_STREAM, stream,
			       devs, num_devs);
	return audio_thread_post_message(thread, &msg.header);
}

int audio_thread_disconnect_stream(struct audio_thread *thread,
				   struct cras_rstream *stream,
				   struct cras_iodev *dev)
{
	struct audio_thread_add_rm_stream_msg msg;

	assert(thread && stream);

	init_add_rm_stream_msg(&msg, AUDIO_THREAD_DISCONNECT_STREAM, stream,
			       &dev, 0);
	return audio_thread_post_message(thread, &msg.header);
}

int audio_thread_drain_stream(struct audio_thread *thread,
			      struct cras_rstream *stream)
{
	struct audio_thread_add_rm_stream_msg msg;

	assert(thread && stream);

	init_add_rm_stream_msg(&msg, AUDIO_THREAD_DRAIN_STREAM, stream,
			       NULL, 0);
	return audio_thread_post_message(thread, &msg.header);
}

int audio_thread_dump_thread_info(struct audio_thread *thread,
				  struct audio_debug_info *info)
{
	struct audio_thread_dump_debug_info_msg msg;

	init_dump_debug_info_msg(&msg, info);
	return audio_thread_post_message(thread, &msg.header);
}

int audio_thread_set_aec_dump(struct audio_thread *thread,
			       cras_stream_id_t stream_id,
			       unsigned int start,
			       int fd)
{
	struct audio_thread_aec_dump_msg msg;

	memset(&msg, 0, sizeof(msg));
	msg.header.id = AUDIO_THREAD_AEC_DUMP;
	msg.header.length = sizeof(msg);
	msg.stream_id = stream_id;
	msg.start = start;
	msg.fd = fd;
	return audio_thread_post_message(thread, &msg.header);
}

int audio_thread_rm_callback_sync(struct audio_thread *thread, int fd) {
	struct audio_thread_rm_callback_msg msg;

	memset(&msg, 0, sizeof(msg));
	msg.header.id = AUDIO_THREAD_REMOVE_CALLBACK;
	msg.header.length = sizeof(msg);
	msg.fd = fd;

	return audio_thread_post_message(thread, &msg.header);
}

int audio_thread_config_global_remix(struct audio_thread *thread,
				     unsigned int num_channels,
				     const float *coefficient)
{
	int err;
	int identity_remix = 1;
	unsigned int i, j;
	struct audio_thread_config_global_remix msg;
	void *rsp;

	init_config_global_remix_msg(&msg);

	/* Check if the coefficients represent an identity matrix for remix
	 * conversion, which means no remix at all. If so then leave the
	 * converter as NULL. */
	for (i = 0; i < num_channels; i++) {
		if (coefficient[i * num_channels + i] != 1.0f) {
			identity_remix = 0;
			break;
		}
		for (j = i + 1; j < num_channels; j++) {
			if (coefficient[i * num_channels + j] != 0 ||
			    coefficient[j * num_channels + i] != 0) {
				identity_remix = 0;
				break;
			}
		}
	}

	if (!identity_remix) {
		msg.fmt_conv = cras_channel_remix_conv_create(num_channels,
							      coefficient);
		if (NULL == msg.fmt_conv)
			return -ENOMEM;
	}

	err = write(thread->to_thread_fds[1], &msg, msg.header.length);
	if (err < 0) {
		syslog(LOG_ERR, "Failed to post message to thread.");
		return err;
	}
	/* Synchronous action, wait for response. */
	err = read_until_finished(thread->to_main_fds[0], &rsp, sizeof(rsp));
	if (err < 0) {
		syslog(LOG_ERR, "Failed to read reply from thread.");
		return err;
	}

	if (rsp)
		cras_fmt_conv_destroy((struct cras_fmt_conv **)&rsp);
	return 0;
}

struct audio_thread *audio_thread_create()
{
	int rc;
	struct audio_thread *thread;

	thread = (struct audio_thread *)calloc(1, sizeof(*thread));
	if (!thread)
		return NULL;

	thread->to_thread_fds[0] = -1;
	thread->to_thread_fds[1] = -1;
	thread->to_main_fds[0] = -1;
	thread->to_main_fds[1] = -1;

	/* Two way pipes for communication with the device's audio thread. */
	rc = pipe(thread->to_thread_fds);
	if (rc < 0) {
		syslog(LOG_ERR, "Failed to pipe");
		free(thread);
		return NULL;
	}
	rc = pipe(thread->to_main_fds);
	if (rc < 0) {
		syslog(LOG_ERR, "Failed to pipe");
		free(thread);
		return NULL;
	}

	atlog = audio_thread_event_log_init();

	thread->pollfds_size = 32;
	thread->pollfds =
		(struct pollfd *)malloc(sizeof(*thread->pollfds)
					* thread->pollfds_size);

	return thread;
}

int audio_thread_add_open_dev(struct audio_thread *thread,
				struct cras_iodev *dev)
{
	struct audio_thread_open_device_msg msg;

	assert(thread && dev);

	if (!thread->started)
		return -EINVAL;

	init_open_device_msg(&msg, AUDIO_THREAD_ADD_OPEN_DEV, dev);
	return audio_thread_post_message(thread, &msg.header);
}

int audio_thread_rm_open_dev(struct audio_thread *thread,
			     struct cras_iodev *dev)
{
	struct audio_thread_open_device_msg msg;

	assert(thread && dev);
	if (!thread->started)
		return -EINVAL;

	init_open_device_msg(&msg, AUDIO_THREAD_RM_OPEN_DEV, dev);
	return audio_thread_post_message(thread, &msg.header);
}

int audio_thread_is_dev_open(struct audio_thread *thread,
			     struct cras_iodev *dev)
{
	struct audio_thread_open_device_msg msg;

	if (!dev)
		return 0;

	init_open_device_msg(&msg, AUDIO_THREAD_IS_DEV_OPEN, dev);
	return audio_thread_post_message(thread, &msg.header);
}

int audio_thread_dev_start_ramp(struct audio_thread *thread,
				struct cras_iodev *dev,
				enum CRAS_IODEV_RAMP_REQUEST request)
{
	struct audio_thread_dev_start_ramp_msg msg;

	assert(thread && dev);

	if (!thread->started)
		return -EINVAL;

	init_device_start_ramp_msg(&msg, AUDIO_THREAD_DEV_START_RAMP,
				   dev, request);
	return audio_thread_post_message(thread, &msg.header);
}

int audio_thread_start(struct audio_thread *thread)
{
	int rc;

	rc = pthread_create(&thread->tid, NULL, audio_io_thread, thread);
	if (rc) {
		syslog(LOG_ERR, "Failed pthread_create");
		return rc;
	}

	thread->started = 1;

	return 0;
}

void audio_thread_destroy(struct audio_thread *thread)
{
	if (thread->started) {
		struct audio_thread_msg msg;

		msg.id = AUDIO_THREAD_STOP;
		msg.length = sizeof(msg);
		audio_thread_post_message(thread, &msg);
		pthread_join(thread->tid, NULL);
	}

	free(thread->pollfds);

	audio_thread_event_log_deinit(atlog);

	if (thread->to_thread_fds[0] != -1) {
		close(thread->to_thread_fds[0]);
		close(thread->to_thread_fds[1]);
	}
	if (thread->to_main_fds[0] != -1) {
		close(thread->to_main_fds[0]);
		close(thread->to_main_fds[1]);
	}

	if (thread->remix_converter)
		cras_fmt_conv_destroy(&thread->remix_converter);

	free(thread);
}