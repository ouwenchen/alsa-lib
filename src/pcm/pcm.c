/*
 *  PCM Interface - main file
 *  Copyright (c) 1998 by Jaroslav Kysela <perex@suse.cz>
 *
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Library General Public License as
 *   published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Library General Public License for more details.
 *
 *   You should have received a copy of the GNU Library General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
  
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "pcm_local.h"

#define SND_FILE_PCM_PLAYBACK		"/dev/snd/pcmC%iD%ip"
#define SND_FILE_PCM_CAPTURE		"/dev/snd/pcmC%iD%ic"
#define SND_PCM_VERSION_MAX	SND_PROTOCOL_VERSION(1, 1, 0)

int snd_pcm_open(snd_pcm_t **handle, int card, int device, int mode)
{
	return snd_pcm_open_subdevice(handle, card, device, -1, mode);
}

static int snd_pcm_open_channel(int card, int device, int channel, int subdevice, int fmode, snd_ctl_t *ctl, int *ver)
{
	char filename[32];
	char *filefmt;
	int err, fd;
	int attempt = 0;
	snd_pcm_channel_info_t info;
	switch (channel) {
	case SND_PCM_CHANNEL_PLAYBACK:
		filefmt = SND_FILE_PCM_PLAYBACK;
		break;
	case SND_PCM_CHANNEL_CAPTURE:
		filefmt = SND_FILE_PCM_CAPTURE;
		break;
	default:
		return -EINVAL;
	}
	if ((err = snd_ctl_pcm_channel_prefer_subdevice(ctl, device, channel, subdevice)) < 0)
		return err;
	sprintf(filename, filefmt, card, device);

      __again:
      	if (attempt++ > 3) {
      		snd_ctl_close(ctl);
      		return -EBUSY;
      	}
	if ((fd = open(filename, fmode)) < 0) {
		err = -errno;
		return err;
	}
	if (ioctl(fd, SND_PCM_IOCTL_PVERSION, ver) < 0) {
		err = -errno;
		close(fd);
		return err;
	}
	if (SND_PROTOCOL_INCOMPATIBLE(*ver, SND_PCM_VERSION_MAX)) {
		close(fd);
		return -SND_ERROR_INCOMPATIBLE_VERSION;
	}
	if (subdevice >= 0) {
		memset(&info, 0, sizeof(info));
		if (ioctl(fd, SND_PCM_IOCTL_CHANNEL_INFO, &info) < 0) {
			err = -errno;
			close(fd);
			return err;
		}
		if (info.subdevice != subdevice) {
			close(fd);
			goto __again;
		}
	}
	return fd;
}

int snd_pcm_open_subdevice(snd_pcm_t **handle, int card, int device, int subdevice, int mode)
{
	int fmode, ver, err;
	snd_pcm_t *pcm;
	snd_ctl_t *ctl;
	int pfd = -1, cfd = -1;

	*handle = NULL;
	
	if (card < 0 || card >= SND_CARDS)
		return -EINVAL;
	if ((err = snd_ctl_open(&ctl, card)) < 0)
		return err;
	fmode = O_RDWR;
	if (mode & SND_PCM_OPEN_NONBLOCK)
		fmode |= O_NONBLOCK;
	if (mode & SND_PCM_OPEN_PLAYBACK) {
		pfd = snd_pcm_open_channel(card, device, SND_PCM_CHANNEL_PLAYBACK,
					  subdevice, fmode, ctl, &ver);
		if (pfd < 0) {
			snd_ctl_close(ctl);
			return pfd;
		}
	}
	if (mode & SND_PCM_OPEN_CAPTURE) {
		cfd = snd_pcm_open_channel(card, device, SND_PCM_CHANNEL_CAPTURE,
					  subdevice, fmode, ctl, &ver);
		if (cfd < 0) {
			if (pfd >= 0)
				close(pfd);
			snd_ctl_close(ctl);
			return cfd;
		}
	}
	snd_ctl_close(ctl);
	if (pfd < 0 && cfd < 0)
		return -EINVAL;
	pcm = (snd_pcm_t *) calloc(1, sizeof(snd_pcm_t));
	if (pcm == NULL) {
		if (pfd >= 0)
			close(pfd);
		if (cfd >= 0)
			close(cfd);
		return -ENOMEM;
	}
	pcm->card = card;
	pcm->device = device;
	pcm->chan[SND_PCM_CHANNEL_PLAYBACK].fd = pfd;
	pcm->chan[SND_PCM_CHANNEL_CAPTURE].fd = cfd;
	pcm->mode = mode;
	pcm->ver = ver;
	*handle = pcm;
	return 0;
}

int snd_pcm_close(snd_pcm_t *pcm)
{
	int res = 0;
	int channel;

	if (!pcm)
		return -EINVAL;
	for (channel = 0; channel < 2; ++channel) {
		snd_pcm_plugin_munmap(pcm, channel);
		snd_pcm_plugin_clear(pcm, channel);
		snd_pcm_munmap(pcm, channel);
		if (pcm->chan[channel].fd >= 0)
			if (close(pcm->chan[channel].fd))
				res = -errno;
	}
	free(pcm);
	return res;
}

int snd_pcm_file_descriptor(snd_pcm_t *pcm, int channel)
{
	if (!pcm)
		return -EINVAL;
	if (channel < 0 || channel > 1)
		return -EINVAL;
	return pcm->chan[channel].fd;
}

int snd_pcm_nonblock_mode(snd_pcm_t *pcm, int nonblock)
{
	long flags;
	int fd, channel;

	if (!pcm)
		return -EINVAL;
	for (channel = 0; channel < 2; ++channel) {
		fd = pcm->chan[channel].fd;
		if (fd < 0)
			continue;
		if ((flags = fcntl(fd, F_GETFL)) < 0)
			return -errno;
		if (nonblock)
			flags |= O_NONBLOCK;
		else
			flags &= ~O_NONBLOCK;
		if (fcntl(fd, F_SETFL, flags) < 0)
			return -errno;
		if (nonblock)
			pcm->mode |= SND_PCM_OPEN_NONBLOCK;
		else
			pcm->mode &= ~SND_PCM_OPEN_NONBLOCK;
	}
	return 0;
}

int snd_pcm_info(snd_pcm_t *pcm, snd_pcm_info_t * info)
{
	int fd, channel;
	if (!pcm || !info)
		return -EINVAL;
	for (channel = 0; channel < 2; ++channel) {
		fd = pcm->chan[channel].fd;
		if (fd >= 0)
			break;
	}
	if (ioctl(fd, SND_PCM_IOCTL_INFO, info) < 0)
		return -errno;
	return 0;
}

int snd_pcm_channel_info(snd_pcm_t *pcm, snd_pcm_channel_info_t * info)
{
	int fd;
	if (!pcm || !info)
		return -EINVAL;
	if (info->channel < 0 || info->channel > 1)
		return -EINVAL;
	fd = pcm->chan[info->channel].fd;
	if (fd < 0)
		return -EINVAL;
	if (ioctl(fd, SND_PCM_IOCTL_CHANNEL_INFO, info) < 0)
		return -errno;
	return 0;
}

int snd_pcm_channel_params(snd_pcm_t *pcm, snd_pcm_channel_params_t * params)
{
	int err;
	int fd;
	struct snd_pcm_chan *chan;

	if (!pcm || !params)
		return -EINVAL;
	if (params->channel < 0 || params->channel > 1)
		return -EINVAL;
	chan = &pcm->chan[params->channel];
	fd = chan->fd;
	if (fd < 0)
		return -EINVAL;
	if (ioctl(fd, SND_PCM_IOCTL_CHANNEL_PARAMS, params) < 0)
		return -errno;
	chan->setup_is_valid = 0;
	memset(&chan->setup, 0, sizeof(snd_pcm_channel_setup_t));
	chan->setup.channel = params->channel;
	if ((err = snd_pcm_channel_setup(pcm, &chan->setup))<0)
		return err;
	return 0;
}

int snd_pcm_channel_setup(snd_pcm_t *pcm, snd_pcm_channel_setup_t * setup)
{
	int fd;
	struct snd_pcm_chan *chan;

	if (!pcm || !setup)
		return -EINVAL;
	if (setup->channel < 0 || setup->channel > 1)
		return -EINVAL;
	chan = &pcm->chan[setup->channel];
	fd = chan->fd;
	if (fd < 0)
		return -EINVAL;
	if (chan->setup_is_valid) {
		memcpy(setup, &chan->setup, sizeof(*setup));
		return 0;
	}
	if (ioctl(fd, SND_PCM_IOCTL_CHANNEL_SETUP, setup) < 0)
		return -errno;
	memcpy(&chan->setup, setup, sizeof(*setup));
	chan->setup_is_valid = 1;
	return 0;
}

int snd_pcm_voice_setup(snd_pcm_t *pcm, int channel, snd_pcm_voice_setup_t * setup)
{
	int fd;
	if (!pcm || !setup)
		return -EINVAL;
	if (channel < 0 || channel > 1)
		return -EINVAL;
	fd = pcm->chan[channel].fd;
	if (fd < 0)
		return -EINVAL;
	if (ioctl(fd, SND_PCM_IOCTL_VOICE_SETUP, setup) < 0)
		return -errno;
	return 0;
}

int snd_pcm_channel_status(snd_pcm_t *pcm, snd_pcm_channel_status_t * status)
{
	int fd;
	if (!pcm || !status)
		return -EINVAL;
	fd = pcm->chan[status->channel].fd;
	if (fd < 0)
		return -EINVAL;
	if (ioctl(fd, SND_PCM_IOCTL_CHANNEL_STATUS, status) < 0)
		return -errno;
	return 0;
}

int snd_pcm_playback_prepare(snd_pcm_t *pcm)
{
	if (!pcm)
		return -EINVAL;
	if (pcm->chan[SND_PCM_CHANNEL_PLAYBACK].fd < 0)
		return -EINVAL;
	if (ioctl(pcm->chan[SND_PCM_CHANNEL_PLAYBACK].fd, SND_PCM_IOCTL_CHANNEL_PREPARE) < 0)
		return -errno;
	return 0;
}

int snd_pcm_capture_prepare(snd_pcm_t *pcm)
{
	if (!pcm)
		return -EINVAL;
	if (pcm->chan[SND_PCM_CHANNEL_CAPTURE].fd < 0)
		return -EINVAL;
	if (ioctl(pcm->chan[SND_PCM_CHANNEL_CAPTURE].fd, SND_PCM_IOCTL_CHANNEL_PREPARE) < 0)
		return -errno;
	return 0;
}

int snd_pcm_channel_prepare(snd_pcm_t *pcm, int channel)
{
	switch (channel) {
	case SND_PCM_CHANNEL_PLAYBACK:
		return snd_pcm_playback_prepare(pcm);
	case SND_PCM_CHANNEL_CAPTURE:
		return snd_pcm_capture_prepare(pcm);
	default:
		return -EIO;
	}
}

int snd_pcm_playback_go(snd_pcm_t *pcm)
{
	if (!pcm)
		return -EINVAL;
	if (pcm->chan[SND_PCM_CHANNEL_PLAYBACK].fd < 0)
		return -EINVAL;
	if (ioctl(pcm->chan[SND_PCM_CHANNEL_PLAYBACK].fd, SND_PCM_IOCTL_CHANNEL_GO) < 0)
		return -errno;
	return 0;
}

int snd_pcm_capture_go(snd_pcm_t *pcm)
{
	if (!pcm)
		return -EINVAL;
	if (pcm->chan[SND_PCM_CHANNEL_CAPTURE].fd < 0)
		return -EINVAL;
	if (ioctl(pcm->chan[SND_PCM_CHANNEL_CAPTURE].fd, SND_PCM_IOCTL_CHANNEL_GO) < 0)
		return -errno;
	return 0;
}

int snd_pcm_channel_go(snd_pcm_t *pcm, int channel)
{
	switch (channel) {
	case SND_PCM_CHANNEL_PLAYBACK:
		return snd_pcm_playback_go(pcm);
	case SND_PCM_CHANNEL_CAPTURE:
		return snd_pcm_capture_go(pcm);
	default:
		return -EIO;
	}
}

int snd_pcm_sync_go(snd_pcm_t *pcm, snd_pcm_sync_t *sync)
{
	if (!pcm || !sync)
		return -EINVAL;
	if (pcm->chan[SND_PCM_CHANNEL_PLAYBACK].fd < 0)
		return -EINVAL;
	if (ioctl(pcm->chan[SND_PCM_CHANNEL_PLAYBACK].fd, SND_PCM_IOCTL_SYNC_GO, sync) < 0)
		return -errno;
	return 0;
}

int snd_pcm_playback_drain(snd_pcm_t *pcm)
{
	if (!pcm)
		return -EINVAL;
	if (pcm->chan[SND_PCM_CHANNEL_PLAYBACK].fd < 0)
		return -EINVAL;
	if (ioctl(pcm->chan[SND_PCM_CHANNEL_PLAYBACK].fd, SND_PCM_IOCTL_CHANNEL_DRAIN) < 0)
		return -errno;
	return 0;
}

int snd_pcm_playback_flush(snd_pcm_t *pcm)
{
	if (!pcm)
		return -EINVAL;
	if (pcm->chan[SND_PCM_CHANNEL_PLAYBACK].fd < 0)
		return -EINVAL;
	if (ioctl(pcm->chan[SND_PCM_CHANNEL_PLAYBACK].fd, SND_PCM_IOCTL_CHANNEL_FLUSH) < 0)
		return -errno;
	return 0;
}

int snd_pcm_capture_flush(snd_pcm_t *pcm)
{
	if (!pcm)
		return -EINVAL;
	if (pcm->chan[SND_PCM_CHANNEL_CAPTURE].fd < 0)
		return -EINVAL;
	if (ioctl(pcm->chan[SND_PCM_CHANNEL_CAPTURE].fd, SND_PCM_IOCTL_CHANNEL_FLUSH) < 0)
		return -errno;
	return 0;
}

int snd_pcm_channel_flush(snd_pcm_t *pcm, int channel)
{
	switch (channel) {
	case SND_PCM_CHANNEL_PLAYBACK:
		return snd_pcm_playback_flush(pcm);
	case SND_PCM_CHANNEL_CAPTURE:
		return snd_pcm_capture_flush(pcm);
	default:
		return -EIO;
	}
}

int snd_pcm_playback_pause(snd_pcm_t *pcm, int enable)
{
	if (!pcm)
		return -EINVAL;
	if (pcm->chan[SND_PCM_CHANNEL_PLAYBACK].fd < 0)
		return -EINVAL;
	if (ioctl(pcm->chan[SND_PCM_CHANNEL_PLAYBACK].fd, SND_PCM_IOCTL_CHANNEL_PAUSE, &enable) < 0)
		return -errno;
	return 0;
}

ssize_t snd_pcm_transfer_size(snd_pcm_t *pcm, int channel)
{
	struct snd_pcm_chan *chan;
	if (!pcm || channel < 0 || channel > 1)
		return -EINVAL;
	chan = &pcm->chan[channel];
	if (!chan->setup_is_valid)
		return -EBADFD;
	if (chan->setup.mode != SND_PCM_MODE_BLOCK)
		return -EBADFD;
	return chan->setup.buf.block.frag_size;
}

ssize_t snd_pcm_write(snd_pcm_t *pcm, const void *buffer, size_t size)
{
	ssize_t result;

	if (!pcm || (!buffer && size > 0) || size < 0)
		return -EINVAL;
	if (pcm->chan[SND_PCM_CHANNEL_PLAYBACK].fd < 0)
		return -EINVAL;
	result = write(pcm->chan[SND_PCM_CHANNEL_PLAYBACK].fd, buffer, size);
	if (result < 0)
		return -errno;
	return result;
}

ssize_t snd_pcm_writev(snd_pcm_t *pcm, const struct iovec *vector, int count)
{
	ssize_t result;

	if (!pcm || (!vector && count > 0) || count < 0)
		return -EINVAL;
	if (pcm->chan[SND_PCM_CHANNEL_PLAYBACK].fd < 0)
		return -EINVAL;
#if 0
	result = writev(pcm->chan[SND_PCM_CHANNEL_PLAYBACK].fd, vector, count);
#else
	{
		snd_v_args_t args;
		args.vector = vector;
		args.count = count;
		result = ioctl(pcm->chan[SND_PCM_CHANNEL_PLAYBACK].fd, SND_IOCTL_WRITEV, &args);
	}
#endif
	if (result < 0)
		return -errno;
	return result;
}

ssize_t snd_pcm_read(snd_pcm_t *pcm, void *buffer, size_t size)
{
	ssize_t result;

	if (!pcm || (!buffer && size > 0) || size < 0)
		return -EINVAL;
	if (pcm->chan[SND_PCM_CHANNEL_CAPTURE].fd < 0)
		return -EINVAL;
	result = read(pcm->chan[SND_PCM_CHANNEL_CAPTURE].fd, buffer, size);
	if (result < 0)
		return -errno;
	return result;
}

ssize_t snd_pcm_readv(snd_pcm_t *pcm, const struct iovec *vector, int count)
{
	ssize_t result;

	if (!pcm || (!vector && count > 0) || count < 0)
		return -EINVAL;
	if (pcm->chan[SND_PCM_CHANNEL_CAPTURE].fd < 0)
		return -EINVAL;
#if 0
	result = readv(pcm->chan[SND_PCM_CHANNEL_CAPTURE].fd, vector, count);
#else
	{
		snd_v_args_t args;
		args.vector = vector;
		args.count = count;
		result = ioctl(pcm->chan[SND_PCM_CHANNEL_CAPTURE].fd, SND_IOCTL_READV, &args);
	}
#endif
	if (result < 0)
		return -errno;
	return result;
}

int snd_pcm_mmap(snd_pcm_t *pcm, int channel, snd_pcm_mmap_control_t **control, void **buffer)
{
	snd_pcm_channel_info_t info;
	int err, fd, prot;
	void *caddr, *daddr;
	struct snd_pcm_chan *chan;

	if (control)
		*control = NULL;
	if (buffer)
		*buffer = NULL;
	if (!pcm || channel < 0 || channel > 1 || !control || !buffer)
		return -EINVAL;
	chan = &pcm->chan[channel];
	fd = chan->fd;
	if (fd < 0)
		return -EINVAL;
	memset(&info, 0, sizeof(info));
	info.channel = channel;
	if ((err = snd_pcm_channel_info(pcm, &info))<0)
		return err;
	caddr = mmap(NULL, sizeof(snd_pcm_mmap_control_t), PROT_READ|PROT_WRITE, MAP_FILE|MAP_SHARED, fd, SND_PCM_MMAP_OFFSET_CONTROL);
	if (caddr == (caddr_t)-1 || caddr == NULL)
		return -errno;
	prot = channel == SND_PCM_CHANNEL_PLAYBACK ? PROT_WRITE : PROT_READ;
	daddr = mmap(NULL, info.mmap_size, prot, MAP_FILE|MAP_SHARED, fd, SND_PCM_MMAP_OFFSET_DATA);
	if (daddr == (caddr_t)-1 || daddr == NULL) {
		err = -errno;
		munmap(caddr, sizeof(snd_pcm_mmap_control_t));
		return err;
	}
	*control = chan->mmap_control = caddr;
	*buffer = chan->mmap_data = daddr;
	chan->mmap_size = info.mmap_size;
	return 0;
}

int snd_pcm_munmap(snd_pcm_t *pcm, int channel)
{
	struct snd_pcm_chan *chan;
	if (!pcm || channel < 0 || channel > 1)
		return -EINVAL;
	chan = &pcm->chan[channel];
	if (chan->mmap_control) {
		munmap(chan->mmap_control, sizeof(snd_pcm_mmap_control_t));
		chan->mmap_control = NULL;
	}
	if (chan->mmap_data) {
		munmap(chan->mmap_data, chan->mmap_size);
		chan->mmap_data = NULL;
		chan->mmap_size = 0;
	}
	return 0;
}
