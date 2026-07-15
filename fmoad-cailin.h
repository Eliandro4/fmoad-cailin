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

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <libgen.h>
#include "json.h"

#include "fsb.h"
#include "events_db.h"

#define PROJ		"FMOAD-CAILIN"	// project name
#define MAXSTR		1024
#define MAX_INST_SP	1024	// maximum StreamPlayers referenced in eventinstance
#define MAX_BANK_SAMPLES	65536

#define FM_INITFLAGS			unsigned int
#define FM_SOUND			int
#define FM_STUDIO_INITFLAGS		unsigned int
#define FM_STUDIO_LOAD_BANK_FLAGS	unsigned int
#define FM_SYSTEM			int

static bool init_done __attribute__((unused)) = false;
static unsigned int loglevel __attribute__((unused)) = 0;

#define DPRINT(threshold, ...) do { \
	if (threshold <= loglevel) { \
		fprintf(stderr, "[%s] %s: ", PROJ, __func__); \
		fprintf(stderr, __VA_ARGS__); \
		fprintf(stderr, "\n"); \
	} \
} while (0)

#define STUB() do { \
	DPRINT(2, "STUB"); \
	return 0; \
} while (0)

typedef enum {
	STOP_ALLOWFADEOUT,
	STOP_IMMEDIATE
} FM_STOP_MODE;

typedef enum {
	PLAYBACK_PLAYING,
	PLAYBACK_SUSTAINING,
	PLAYBACK_STOPPED,
	PLAYBACK_STARTING,
	PLAYBACK_STOPPING
} FM_PLAYBACK_STATE;

typedef enum {
	LOAD_MEMORY,
	LOAD_MEMORY_POINT
} FM_LOAD_MEMORY_MODE;

typedef struct FMOD_VECTOR {
	float x;
	float y;
	float z;
} FMOD_VECTOR;

typedef struct FM_3D_ATTRIBUTES{
	FMOD_VECTOR position;
	FMOD_VECTOR velocity;
	FMOD_VECTOR forward;
	FMOD_VECTOR up;
} FM_3D_ATTRIBUTES;

typedef struct BANK{
	const char *name;
	const char *parentdir;
	const char *bankpath;
	json_value *jo;
	const char *path;	// the FMOD bank internal path, like "bank:/Master Bank"
	const char *guid;
	bool native_loaded;
	int sample_count;
	char **sample_names;
	uint8_t **sample_ogg_data;
	size_t *sample_ogg_len;
	/* Native event->sample mapping (from the embedded events DB). */
	struct bank_events events;
} BANK;

typedef struct SYSTEM{
	unsigned int maxchannels;
	unsigned int flags;
	unsigned int studioflags;
} SYSTEM;

typedef struct VCA{
	const char *path;
	float volume;
	float finalvolume;
} VCA;

typedef struct BUS{
	const char *path;
	bool paused;
	float volume;
	bool muted;
} BUS;

typedef struct EVENTDESCRIPTION{
	const char *path;
	int sound_idx;		// index of the sound_object in the sounds array
} EVENTDESCRIPTION;

typedef struct EVENTINSTANCE{
	EVENTDESCRIPTION *evd;
	int n_sp;			// number of StreamPlayers in sp_idx
	int sp_idx[MAX_INST_SP];	// array of integers referring to StreamPlayers
	float volume;
	float finalvolume;
	bool paused;
	bool released;
	bool looping;
	float pitch;
	FM_3D_ATTRIBUTES attributes;
} EVENTINSTANCE;

int FMOD_Studio_Bank_LoadSampleData(BANK *bank);
int FMOD_Studio_Bus_GetPaused(BUS *bus, bool *paused);
int FMOD_Studio_Bus_SetPaused(BUS *bus, bool paused);
int FMOD_Studio_Bus_StopAllEvents(BUS *bus, FM_STOP_MODE mode);
int FMOD_Studio_EventDescription_CreateInstance(EVENTDESCRIPTION *eventdescription, EVENTINSTANCE **instance);
int FMOD_Studio_EventDescription_GetPath(EVENTDESCRIPTION *eventdescription, char *path, int size, int *retrieved);
int FMOD_Studio_EventDescription_Is3D(EVENTDESCRIPTION *eventdescription, bool *is3D);
int FMOD_Studio_EventDescription_IsOneshot(EVENTDESCRIPTION *eventdescription, bool *oneshot);
int FMOD_Studio_EventDescription_LoadSampleData(EVENTDESCRIPTION *eventdescription);
int FMOD_Studio_EventInstance_Get3DAttributes(EVENTINSTANCE *eventinstance, FM_3D_ATTRIBUTES *attributes);
int FMOD_Studio_EventInstance_GetDescription(EVENTINSTANCE *eventinstance, EVENTDESCRIPTION **description);
int FMOD_Studio_EventInstance_GetPaused(EVENTINSTANCE *eventinstance, bool *paused);
int FMOD_Studio_EventInstance_GetPlaybackState(EVENTINSTANCE *eventinstance, int *state);
int FMOD_Studio_EventInstance_GetVolume(EVENTINSTANCE *eventinstance, float *volume, float *finalvolume);
int FMOD_Studio_EventInstance_Release(EVENTINSTANCE *eventinstance);
int FMOD_Studio_EventInstance_Set3DAttributes(EVENTINSTANCE *eventinstance, FM_3D_ATTRIBUTES *attributes);
int FMOD_Studio_EventInstance_SetParameterValue(EVENTINSTANCE *eventinstance, char *name, float value);
int FMOD_Studio_EventInstance_SetPaused(EVENTINSTANCE *eventinstance, bool paused);
int FMOD_Studio_EventInstance_SetVolume(EVENTINSTANCE *eventinstance, float volume);
int FMOD_Studio_EventInstance_Start(EVENTINSTANCE *eventinstance);
int FMOD_Studio_EventInstance_Stop(EVENTINSTANCE *eventinstance, FM_STOP_MODE mode);
int FMOD_Studio_EventInstance_TriggerCue(EVENTINSTANCE *eventinstance);
int FMOD_Studio_System_Create(SYSTEM **system, unsigned int headerversion);
int FMOD_Studio_System_GetBus(SYSTEM *system, const char *path, BUS **bus);
int FMOD_Studio_System_GetEvent(SYSTEM *system, const char *path, EVENTDESCRIPTION **event);
int FMOD_Studio_System_GetLowLevelSystem(SYSTEM *system, void **lowLevelSystem);
int FMOD_Studio_System_GetListenerAttributes(SYSTEM *system, int listener, FM_3D_ATTRIBUTES *attributes);
int FMOD_Studio_System_GetVCA(SYSTEM *system, const char *path, VCA **vca);
int FMOD_Studio_System_Initialize(SYSTEM *system, int maxchannels, FM_STUDIO_INITFLAGS studioflags, FM_INITFLAGS flags, void *extradriverdata);
int FMOD_Studio_System_LoadBankFile(SYSTEM *system, const char *filename, FM_STUDIO_LOAD_BANK_FLAGS flags, BANK **bank);
int FMOD_Studio_System_LoadBankMemory(SYSTEM *system, const char *buffer, int length, int mode, FM_STUDIO_LOAD_BANK_FLAGS flags, BANK **bank);
int FMOD_Studio_System_Release(SYSTEM *system);
int FMOD_Studio_System_SetListenerAttributes(SYSTEM *system, int listener, FM_3D_ATTRIBUTES *attributes);
int FMOD_Studio_System_Update(SYSTEM *system);
int FMOD_Studio_VCA_SetVolume(VCA *vca, float volume);
int FMOD_Studio_VCA_GetVolume(VCA *vca, float *volume, float *finalvolume);

int get_sound_idx(char *path);

VCA *NewVca(void);

/* Bus and VCA registries, keyed by their FMOD path (e.g. "bus:/music").
 * Celeste queries the same path repeatedly (e.g. every frame for volume
 * sliders and pause flags), so the state must persist across calls. */
BUS *find_or_create_bus(const char *path);
VCA *find_or_create_vca(const char *path);

/* EventInstance lifecycle registry.  FMOD's release() only *marks* an
 * instance for destruction once it has finished playing; the C# wrapper
 * keeps using the same raw pointer afterwards (e.g. Stop() after a
 * start()+release() one-shot).  So we must not free the struct up front.
 * Released instances are freed by System_Update once all of their
 * StreamPlayers have been retired. */
void register_instance(EVENTINSTANCE *instance);
void free_instance(EVENTINSTANCE *instance);

/* Recompute OpenAL gain (bus * VCA * event volume) and pause state for
 * every active StreamPlayer based on the current bus/VCA graph and the
 * event's FMOD path.  Called whenever any bus/VCA/event volume or pause
 * flag changes. */
void apply_audio_state(void);

int bank_load_native(BANK *bank, const char *filename);
int bank_extract_fsb5(BANK *bank, const uint8_t *bank_data, size_t bank_size);
void bank_free_native(BANK *bank);

int FMOD_Studio_System_LoadBankCustom(SYSTEM * studiosystem, void * info, void * flags, void * bank);
int FMOD_Studio_System_UnloadAll(SYSTEM * studiosystem);
int FMOD_Studio_System_GetBank(SYSTEM * studiosystem, char * path, void * bank);
int FMOD_Studio_System_GetBankByID(SYSTEM * studiosystem, void * guid, void * bank);
int FMOD_Studio_System_GetBankCount(SYSTEM * studiosystem, int * count);
int FMOD_Studio_System_GetBankList(SYSTEM * studiosystem, void * array, int capacity, int * count);
int FMOD_Studio_System_GetBusByID(SYSTEM * studiosystem, void * guid, void * bus);
int FMOD_Studio_System_GetEventByID(SYSTEM * studiosystem, void * guid, void * description);
int FMOD_Studio_System_GetVCAByID(SYSTEM * studiosystem, void * guid, void * vca);
int FMOD_Studio_System_LookupID(SYSTEM * studiosystem, char * path, void * guid);
int FMOD_Studio_System_LookupPath(SYSTEM * studiosystem, void * guid, void * path, int size, int * retrieved);
int FMOD_Studio_System_GetSoundInfo(SYSTEM * studiosystem, char * key, void * info);
int FMOD_Studio_System_GetUserData(SYSTEM * studiosystem, void * userdata);
int FMOD_Studio_System_SetUserData(SYSTEM * studiosystem, void * userdata);
int FMOD_Studio_System_GetAdvancedSettings(SYSTEM * studiosystem, void * settings);
int FMOD_Studio_System_SetAdvancedSettings(SYSTEM * studiosystem, void * settings);
int FMOD_Studio_System_GetBufferUsage(SYSTEM * studiosystem, void * usage);
int FMOD_Studio_System_ResetBufferUsage(SYSTEM * studiosystem);
int FMOD_Studio_System_GetCPUUsage(SYSTEM * studiosystem, void * usage);
int FMOD_Studio_System_GetNumListeners(SYSTEM * studiosystem, int * numlisteners);
int FMOD_Studio_System_GetListenerWeight(SYSTEM * studiosystem, int listener, float * weight);
int FMOD_Studio_System_SetNumListeners(SYSTEM * studiosystem, int numlisteners);
int FMOD_Studio_System_SetListenerWeight(SYSTEM * studiosystem, int listener, float weight);
int FMOD_Studio_System_FlushCommands(SYSTEM * studiosystem);
int FMOD_Studio_System_FlushSampleLoading(SYSTEM * studiosystem);
int FMOD_Studio_System_SetCallback(SYSTEM * studiosystem, void * callback, void * callbackmask);
int FMOD_Studio_System_StartCommandCapture(SYSTEM * studiosystem, char * path, void * flags);
int FMOD_Studio_System_StopCommandCapture(SYSTEM * studiosystem);
int FMOD_Studio_System_IsValid(SYSTEM * studiosystem);
int FMOD_Studio_Bank_Unload(BANK * bank);
int FMOD_Studio_Bank_UnloadSampleData(BANK * bank);
int FMOD_Studio_Bank_IsValid(BANK * bank);
int FMOD_Studio_Bank_GetBusCount(BANK * bank, int * count);
int FMOD_Studio_Bank_GetBusList(BANK * bank, void * array, int capacity, int * count);
int FMOD_Studio_Bank_GetEventCount(BANK * bank, int * count);
int FMOD_Studio_Bank_GetEventList(BANK * bank, void * array, int capacity, int * count);
int FMOD_Studio_Bank_GetID(BANK * bank, void * id);
int FMOD_Studio_Bank_GetLoadingState(BANK * bank, void * state);
int FMOD_Studio_Bank_GetPath(BANK * bank, void * path, int size, int * retrieved);
int FMOD_Studio_Bank_GetSampleLoadingState(BANK * bank, void * state);
int FMOD_Studio_Bank_GetStringCount(BANK * bank, int * count);
int FMOD_Studio_Bank_GetStringInfo(BANK * bank, int index, void * id, void * path, int size, int * retrieved);
int FMOD_Studio_Bank_GetUserData(BANK * studiosystem, void * userdata);
int FMOD_Studio_Bank_GetVCACount(BANK * bank, int * count);
int FMOD_Studio_Bank_GetVCAList(BANK * bank, void * array, int capacity, int * count);
int FMOD_Studio_Bank_SetUserData(BANK * studiosystem, void * userdata);
int FMOD_Studio_Bus_StopAllEvents(BUS * bus, FM_STOP_MODE mode);
int FMOD_Studio_Bus_SetPaused(BUS * bus, bool paused);
int FMOD_Studio_Bus_GetPaused(BUS * bus, bool * paused);
int FMOD_Studio_Bus_SetMute(BUS * bus, bool mute);
int FMOD_Studio_Bus_GetMute(BUS * bus, bool * mute);
int FMOD_Studio_Bus_SetVolume(BUS * bus, float volume);
int FMOD_Studio_Bus_GetVolume(BUS * bus, float * volume, float * finalvolume);
int FMOD_Studio_Bus_GetID(BUS * bus, void * id);
int FMOD_Studio_Bus_GetPath(BUS * bus, void * path, int size, int * retrieved);
int FMOD_Studio_Bus_GetChannelGroup(BUS * bus, void * group);
int FMOD_Studio_Bus_LockChannelGroup(BUS * bus);
int FMOD_Studio_Bus_UnlockChannelGroup(BUS * bus);
int FMOD_Studio_Bus_IsValid(BUS * bus);
int FMOD_Studio_EventDescription_CreateInstance(EVENTDESCRIPTION * eventdescription, EVENTINSTANCE ** instance);
int FMOD_Studio_EventDescription_GetID(EVENTDESCRIPTION * eventdescription, void * id);
int FMOD_Studio_EventDescription_GetInstanceCount(EVENTDESCRIPTION * eventdescription, int * count);
int FMOD_Studio_EventDescription_GetInstanceList(EVENTDESCRIPTION * eventdescription, void * array, int capacity, int * count);
int FMOD_Studio_EventDescription_GetLength(EVENTDESCRIPTION * eventdescription, int * length);
int FMOD_Studio_EventDescription_GetMaximumDistance(EVENTDESCRIPTION * eventdescription, float * distance);
int FMOD_Studio_EventDescription_GetMinimumDistance(EVENTDESCRIPTION * eventdescription, float * distance);
int FMOD_Studio_EventDescription_GetParameterByIndex(EVENTDESCRIPTION * eventdescription, int index, void * parameter);
int FMOD_Studio_EventDescription_GetParameterCount(EVENTDESCRIPTION * eventdescription, int * count);
int FMOD_Studio_EventDescription_GetParameter(EVENTDESCRIPTION * eventdescription, char * name, void * parameter);
int FMOD_Studio_EventDescription_GetSampleLoadingState(EVENTDESCRIPTION * eventdescription, void * state);
int FMOD_Studio_EventDescription_GetSoundSize(EVENTDESCRIPTION * eventdescription, float * size);
int FMOD_Studio_EventDescription_GetUserData(EVENTDESCRIPTION * eventdescription, void * userdata);
int FMOD_Studio_EventDescription_GetUserPropertyByIndex(EVENTDESCRIPTION * eventdescription, int index, void * property);
int FMOD_Studio_EventDescription_GetUserPropertyCount(EVENTDESCRIPTION * eventdescription, int * count);
int FMOD_Studio_EventDescription_GetUserProperty(EVENTDESCRIPTION * eventdescription, char * name, void * property);
int FMOD_Studio_EventDescription_HasCue(EVENTDESCRIPTION * eventdescription, bool * cue);
int FMOD_Studio_EventDescription_IsSnapshot(EVENTDESCRIPTION * eventdescription, bool * snapshot);
int FMOD_Studio_EventDescription_IsStream(EVENTDESCRIPTION * eventdescription, bool * isStream);
int FMOD_Studio_EventDescription_IsValid(EVENTDESCRIPTION * eventdescription);
int FMOD_Studio_EventDescription_ReleaseAllInstances(EVENTDESCRIPTION * eventdescription);
int FMOD_Studio_EventDescription_SetCallback(EVENTDESCRIPTION * eventdescription, void * callback, void * callbackmask);
int FMOD_Studio_EventDescription_SetUserData(EVENTDESCRIPTION * eventdescription, void * userdata);
int FMOD_Studio_EventDescription_UnloadSampleData(EVENTDESCRIPTION * eventdescription);
int FMOD_Studio_EventInstance_Start(EVENTINSTANCE * eventinstance);
int FMOD_Studio_EventInstance_Stop(EVENTINSTANCE * eventinstance, FM_STOP_MODE mode);
int FMOD_Studio_EventInstance_GetDescription(EVENTINSTANCE * eventinstance, EVENTDESCRIPTION ** description);
int FMOD_Studio_EventInstance_GetVolume(EVENTINSTANCE * eventinstance, float * volume, float * finalvolume);
int FMOD_Studio_EventInstance_SetVolume(EVENTINSTANCE * eventinstance, float volume);
int FMOD_Studio_EventInstance_GetPitch(EVENTINSTANCE * eventinstance, float * pitch, float * finalpitch);
int FMOD_Studio_EventInstance_SetPitch(EVENTINSTANCE * eventinstance, float pitch);
int FMOD_Studio_EventInstance_Get3DAttributes(EVENTINSTANCE * eventinstance, FM_3D_ATTRIBUTES *attributes);
int FMOD_Studio_EventInstance_Set3DAttributes(EVENTINSTANCE * eventinstance, FM_3D_ATTRIBUTES *attributes);
int FMOD_Studio_EventInstance_GetListenerMask(EVENTINSTANCE * _event, unsigned int * mask);
int FMOD_Studio_EventInstance_SetListenerMask(EVENTINSTANCE * _event, unsigned int mask);
int FMOD_Studio_EventInstance_GetProperty(EVENTINSTANCE * _event, void * index, float * value);
int FMOD_Studio_EventInstance_SetProperty(EVENTINSTANCE * _event, void * index, float value);
int FMOD_Studio_EventInstance_GetReverbLevel(EVENTINSTANCE * _event, int index, float * level);
int FMOD_Studio_EventInstance_SetReverbLevel(EVENTINSTANCE * _event, int index, float level);
int FMOD_Studio_EventInstance_GetPaused(EVENTINSTANCE * eventinstance, bool * paused);
int FMOD_Studio_EventInstance_SetPaused(EVENTINSTANCE * eventinstance, bool paused);
int FMOD_Studio_EventInstance_GetPlaybackState(EVENTINSTANCE * eventinstance, int * state);
int FMOD_Studio_EventInstance_GetTimelinePosition(EVENTINSTANCE * _event, int * position);
int FMOD_Studio_EventInstance_SetTimelinePosition(EVENTINSTANCE * _event, int position);
int FMOD_Studio_EventInstance_GetUserData(EVENTINSTANCE * _event, void * userdata);
int FMOD_Studio_EventInstance_SetUserData(EVENTINSTANCE * _event, void * userdata);
int FMOD_Studio_EventInstance_IsVirtual(EVENTINSTANCE * _event, bool * virtualstate);
int FMOD_Studio_EventInstance_Release(EVENTINSTANCE * eventinstance);
int FMOD_Studio_EventInstance_GetParameter(EVENTINSTANCE * _event, char * name, void * parameter);
int FMOD_Studio_EventInstance_GetParameterByIndex(EVENTINSTANCE * _event, int index, void * parameter);
int FMOD_Studio_EventInstance_GetParameterCount(EVENTINSTANCE * _event, int * count);
int FMOD_Studio_EventInstance_GetParameterValue(EVENTINSTANCE * _event, char * name, float * value, float * finalvalue);
int FMOD_Studio_EventInstance_GetParameterValueByIndex(EVENTINSTANCE * _event, int index, float * value, float * finalvalue);
int FMOD_Studio_EventInstance_SetParameterValue(EVENTINSTANCE * eventinstance, char * name, float value);
int FMOD_Studio_EventInstance_SetParameterValueByIndex(EVENTINSTANCE * _event, int index, float value);
int FMOD_Studio_EventInstance_SetParameterValuesByIndices(EVENTINSTANCE * _event, void * indices, void * values, int count);
int FMOD_Studio_EventInstance_TriggerCue(EVENTINSTANCE * eventinstance);
int FMOD_Studio_EventInstance_SetCallback(EVENTINSTANCE * _event, void * callback, void * callbackmask);
int FMOD_Studio_EventInstance_GetChannelGroup(EVENTINSTANCE * _event, void * group);
int FMOD_Studio_EventInstance_IsValid(EVENTINSTANCE * _event);
int FMOD_Studio_VCA_GetID(VCA * vca, void * id);
int FMOD_Studio_VCA_GetPath(VCA * vca, void * path, int size, int * retrieved);
int FMOD_Studio_VCA_IsValid(VCA * vca);
int FMOD_Studio_ParameterInstance_GetDescription(void * parameter, void * description);
int FMOD_Studio_ParameterInstance_GetValue(void * parameter, float * value);
int FMOD_Studio_ParameterInstance_SetValue(void * parameter, float value);
int FMOD_Studio_ParameterInstance_IsValid(void * parameter);
int FMOD_Studio_CommandReplay_GetCommandAtTime(void * replay, float time, int * commandIndex);
int FMOD_Studio_CommandReplay_GetCommandCount(void * replay, int * count);
int FMOD_Studio_CommandReplay_GetCommandInfo(void * replay, int commandIndex, void * info);
int FMOD_Studio_CommandReplay_GetCommandString(void * replay, int commandIndex, void * description, int capacity);
int FMOD_Studio_CommandReplay_GetCurrentCommand(void * replay, int * commandIndex, float * currentTime);
int FMOD_Studio_CommandReplay_GetLength(void * replay, float * totalTime);
int FMOD_Studio_CommandReplay_GetPaused(void * replay, bool * paused);
int FMOD_Studio_CommandReplay_GetPlaybackState(void * replay, void * state);
int FMOD_Studio_CommandReplay_GetSystem(void * replay, void * system);
int FMOD_Studio_CommandReplay_GetUserData(void * replay, void * userdata);
int FMOD_Studio_CommandReplay_IsValid(void * replay);
int FMOD_Studio_CommandReplay_Release(void * replay);
int FMOD_Studio_CommandReplay_SeekToCommand(void * replay, int commandIndex);
int FMOD_Studio_CommandReplay_SeekToTime(void * replay, float time);
int FMOD_Studio_CommandReplay_SetBankPath(void * replay, char * bankPath);
int FMOD_Studio_CommandReplay_SetCreateInstanceCallback(void * replay, void * callback);
int FMOD_Studio_CommandReplay_SetFrameCallback(void * replay, void * callback);
int FMOD_Studio_CommandReplay_SetLoadBankCallback(void * replay, void * callback);
int FMOD_Studio_CommandReplay_SetPaused(void * replay, bool paused);
int FMOD_Studio_CommandReplay_SetUserData(void * replay, void * userdata);
int FMOD_Studio_CommandReplay_Start(void * replay);
int FMOD_Studio_CommandReplay_Stop(void * replay);
int FMOD_Studio_ParseID(char * idString, void * guid);
