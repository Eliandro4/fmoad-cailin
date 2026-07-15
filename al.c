/*
 * Copyright (c) 2021 Thomas Frohwein
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "al.h"
#include <string.h>

SoundObject sounds[MAXSOUNDS];
unsigned int sound_counter = 0;

StreamPlayer StreamPlayerArr[MAXSOUNDS];
unsigned int sp_counter = 0;

static ALCdevice *alc_device = NULL;
static ALCcontext *alc_ctx = NULL;

int al_init(void)
{
	const ALCchar *name;
	ALCdevice *device;
	ALCcontext *ctx;

	device = NULL;
	device = alcOpenDevice(NULL);	// select the default device
	if(!device)
	{
		fprintf(stderr, "Could not open the OpenAL device!\n");
		return 1;
	}
	ctx = alcCreateContext(device, NULL);
	if(ctx == NULL || alcMakeContextCurrent(ctx) == ALC_FALSE)
	{
		if(ctx != NULL)
			alcDestroyContext(ctx);
		alcCloseDevice(device);
		fprintf(stderr, "Could not set a context!\n");
		return 1;
	}
	alc_device = device;
	alc_ctx = ctx;
	name = NULL;
	if(alcIsExtensionPresent(device, "ALC_ENUMERATE_ALL_EXT"))
		name = alcGetString(device, ALC_ALL_DEVICES_SPECIFIER);
	if(!name || alcGetError(device) != AL_NO_ERROR)
		name = alcGetString(device, ALC_DEVICE_SPECIFIER);
	printf("Opened \"%s\"\n", name);

	return 0;
}

// from openal-soft's alstream.c example
StreamPlayer *NewPlayer(void)
{
	StreamPlayer *player;
	player = malloc(sizeof(*player));
	if (player == NULL)
		return NULL;
	player->status = STOPPED;
	player->released = false;
	player->retired = false;
	player->looping = false;
	player->volume = 1.0f;
	player->fp = NULL;
	player->ov_file = NULL;
	player->membuf = NULL;
	player->mem_data = NULL;
	player->mem_len = 0;
	player->mem_pos = 0;
	alGenBuffers(NUM_BUFFERS, player->buffers);
	if (alGetError() != AL_NO_ERROR) {
		free(player);
		fprintf(stderr, "Could not create buffers (OpenAL limit reached)\n");
		return NULL;
	}
	alGenSources(1, &player->source);
	if (alGetError() != AL_NO_ERROR) {
		alDeleteBuffers(NUM_BUFFERS, player->buffers);
		free(player);
		fprintf(stderr, "Could not create source (OpenAL limit reached)\n");
		return NULL;
	}

	/* Set parameters so mono sources play out the front-center speaker and
	 * won't distance attenuate. */
	alSource3i(player->source, AL_POSITION, 0, 0, -1);
	alSourcei(player->source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSourcei(player->source, AL_ROLLOFF_FACTOR, 0);
	if (alGetError() != AL_NO_ERROR) {
		alDeleteSources(1, &player->source);
		alDeleteBuffers(NUM_BUFFERS, player->buffers);
		free(player);
		return NULL;
	}

	return player;
}

SoundObject *NewSoundObject(void)
{
	SoundObject *so;
	so = malloc(sizeof(*so));
	assert(so != NULL);
	so->n_filepaths = 0;
	so->mem_data = NULL;
	so->mem_len = NULL;
	so->path = "\0";
	return so;
}

// from openal-soft's alstream.c example
void DeletePlayer(StreamPlayer *player)
{
	alDeleteSources(1, &player->source);
	alDeleteBuffers(NUM_BUFFERS, player->buffers);
	if(alGetError() != AL_NO_ERROR)
		fprintf(stderr, "Failed to delete object IDs\n");
	player->released = true;
	player->retired = true;
	ClosePlayerFile(player);
}

// from openal-soft's alstream.c example
int StartPlayer(StreamPlayer *player)
{
	ALsizei i;
	int current_section;

	/* Rewind the source position and clear the buffer queue */
	alSourceRewind(player->source);
	alSourcei(player->source, AL_BUFFER, 0);

	/* Fill the buffer queue */
	for(i = 0; i < NUM_BUFFERS; i++)
	{
		/* Get some data to give it to the buffer */
		long ov_len = ov_read(player->ov_file, player->membuf, BUFFER_SAMPLES, 0, 2, 1, &current_section);
		if(ov_len < 1) break;

		alBufferData(player->buffers[i], player->format, player->membuf, (ALsizei)ov_len,
			player->ov_info.rate);
	}
	if(alGetError() != AL_NO_ERROR)
	{
		fprintf(stderr, "Error buffering for playback\n");
		return 0;
	}

	/* Now queue and start playback! */
	alSourceQueueBuffers(player->source, i, player->buffers);
	alSourcePlay(player->source);
	if(alGetError() != AL_NO_ERROR)
	{
		fprintf(stderr, "Error starting playback\n");
		return 0;
	}

	return 1;
}

int StopPlayer(StreamPlayer *player)
{
	alSourceStop(player->source);
	if (alGetError() != AL_NO_ERROR)
	{
		fprintf(stderr, "Error stopping playback\n");
		return 0;
	}
	return 1;
}

int UpdatePlayer(StreamPlayer *player)
{
	ALint processed, state;
	int current_section;

	/* Get relevant source info */
	alGetSourcei(player->source, AL_SOURCE_STATE, &state);
	alGetSourcei(player->source, AL_BUFFERS_PROCESSED, &processed);
	if(alGetError() != AL_NO_ERROR)
	{
		fprintf(stderr, "Error checking source state\n");
		return 0;
	}

	/* Unqueue and handle each processed buffer */
	while(processed > 0)
	{
		ALuint bufid;
		long ov_len;

		alSourceUnqueueBuffers(player->source, 1, &bufid);
		processed--;

		/* Read the next chunk of data, refill the buffer, and queue it
		 * back on the source */
		ov_len = ov_read(player->ov_file, player->membuf, BUFFER_SAMPLES, 0, 2, 1, &current_section);
		if(ov_len > 0)
		{
			//ov_len *= player->ov_info.channels * (long)sizeof(char);	// this distorts audio
			alBufferData(bufid, player->format, player->membuf, (ALsizei)ov_len,
				player->ov_info.rate);
			alSourceQueueBuffers(player->source, 1, &bufid);
		}
		else if (player->looping)
		{
			/* End of sample reached: restart decoding from the
			 * beginning (clean re-open, not ov_pcm_seek) so the track
			 * repeats seamlessly without corrupted audio. */
			if (ReopenVorbis(player) &&
			    (ov_len = ov_read(player->ov_file, player->membuf, BUFFER_SAMPLES, 0, 2, 1, &current_section)) > 0)
			{
				alBufferData(bufid, player->format, player->membuf, (ALsizei)ov_len,
					player->ov_info.rate);
				alSourceQueueBuffers(player->source, 1, &bufid);
			}
		}
		if(alGetError() != AL_NO_ERROR)
		{
			fprintf(stderr, "Error buffering data\n");
			return 0;
		}
	}

	/* Make sure the source hasn't underrun */
	if(state != AL_PLAYING && state != AL_PAUSED)
	{
		ALint queued;

		/* If no buffers are queued, playback is finished */
		alGetSourcei(player->source, AL_BUFFERS_QUEUED, &queued);
		if(queued == 0)
			return 0;

		alSourcePlay(player->source);
		if(alGetError() != AL_NO_ERROR)
		{
			fprintf(stderr, "Error restarting playback\n");
			return 0;
		}
	}

	return 1;
}

static size_t mem_ov_read(void *ptr, size_t size, size_t nmemb, void *datasource)
{
	StreamPlayer *p = (StreamPlayer *)datasource;
	size_t avail = p->mem_len - p->mem_pos;
	size_t need = size * nmemb;
	if (need > avail)
		need = avail;
	memcpy(ptr, p->mem_data + p->mem_pos, need);
	p->mem_pos += need;
	return need / size;
}

static int mem_ov_seek(void *datasource, ogg_int64_t offset, int whence)
{
	StreamPlayer *p = (StreamPlayer *)datasource;
	size_t new_pos;
	switch (whence) {
	case SEEK_SET:
		new_pos = (size_t)offset;
		break;
	case SEEK_CUR:
		new_pos = p->mem_pos + (size_t)offset;
		break;
	case SEEK_END:
		new_pos = p->mem_len + (size_t)offset;
		break;
	default:
		return -1;
	}
	if (new_pos > p->mem_len)
		return -1;
	p->mem_pos = new_pos;
	return 0;
}

static int mem_ov_close(void *datasource)
{
	(void)datasource;
	return 0;
}

static long mem_ov_tell(void *datasource)
{
	StreamPlayer *p = (StreamPlayer *)datasource;
	return (long)p->mem_pos;
}

/* Opens the first audio stream from an in-memory OGG buffer. */
int OpenPlayerFile(StreamPlayer *player, const uint8_t *data, size_t len)
{
	size_t frame_size;
	player->mem_data = (uint8_t *)data;
	player->mem_len = len;
	player->mem_pos = 0;

	player->ov_file = (OggVorbis_File *)malloc(sizeof(OggVorbis_File));

	static ov_callbacks mem_cbs;
	memset(&mem_cbs, 0, sizeof(mem_cbs));
	mem_cbs.read_func = mem_ov_read;
	mem_cbs.seek_func = mem_ov_seek;
	mem_cbs.close_func = mem_ov_close;
	mem_cbs.tell_func = mem_ov_tell;

	if(ov_open_callbacks(player, player->ov_file, NULL, 0, mem_cbs) < 0)
	{
		fprintf(stderr, "Could not open audio in memory\n");
		free(player->ov_file);
		player->ov_file = NULL;
		return 0;
	}

	player->ov_info = *ov_info(player->ov_file, -1);

	/* Get the sound format, and figure out the OpenAL format */
	if(player->ov_info.channels == 1)
		player->format = AL_FORMAT_MONO16;
	else
		player->format = AL_FORMAT_STEREO16;
	if(!player->format)
	{
		fprintf(stderr, "Unsupported channel count: %d\n", player->ov_info.channels);
		free(player->ov_file);
		player->ov_file = NULL;
		return 0;
	}

	frame_size = (size_t)(BUFFER_SAMPLES * player->ov_info.channels) * sizeof(char);
	player->membuf = malloc(frame_size);

	return 1;
}

/* Restart decoding of the same in-memory OGG from the very beginning.
 * Re-opening is preferred over ov_pcm_seek() here because the latter
 * desynchronises the decoder from our custom read/seek callbacks and
 * yields corrupted ("garbage") audio at the loop point. */
int ReopenVorbis(StreamPlayer *player)
{
	static ov_callbacks mem_cbs;
	memset(&mem_cbs, 0, sizeof(mem_cbs));
	mem_cbs.read_func = mem_ov_read;
	mem_cbs.seek_func = mem_ov_seek;
	mem_cbs.close_func = mem_ov_close;
	mem_cbs.tell_func = mem_ov_tell;

	if (player->ov_file)
		ov_clear(player->ov_file);

	player->mem_pos = 0;
	if (ov_open_callbacks(player, player->ov_file, NULL, 0, mem_cbs) < 0) {
		fprintf(stderr, "Could not reopen looping audio in memory\n");
		return 0;
	}
	player->ov_info = *ov_info(player->ov_file, -1);
	return 1;
}

/* Closes the audio stream */
void ClosePlayerFile(StreamPlayer *player)
{
	if(player->ov_file) {
		ov_clear(player->ov_file);
		free(player->ov_file);
		player->ov_file = NULL;
	}
	if(player->membuf)
	{
		free(player->membuf);
		player->membuf = NULL;
	}
	player->mem_data = NULL;
	player->mem_len = 0;
	player->mem_pos = 0;
}

/* Tears down the OpenAL context and device opened by al_init(). */
void al_shutdown(void)
{
	if (alc_ctx != NULL) {
		alcMakeContextCurrent(NULL);
		alcDestroyContext(alc_ctx);
		alc_ctx = NULL;
	}
	if (alc_device != NULL) {
		alcCloseDevice(alc_device);
		alc_device = NULL;
	}
}
