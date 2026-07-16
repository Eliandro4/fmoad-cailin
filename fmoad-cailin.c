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

#include "fmoad-cailin.h"
#include "al.h"
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

int get_sound_idx(char *path)
{
	for (int i = 0; i < sound_counter; i++)
	{
		if (!sounds[i].path)	// skip if an empty entry
			continue;
		if (!strncmp(path, sounds[i].path, MAXSTR))
			return i;
	}
	return -1;	// not found, this is an error
}

/* Read an entire file into a freshly allocated buffer (caller frees). */
static uint8_t *read_file_bytes(const char *filename, size_t *out_len)
{
	FILE *fp = fopen(filename, "rb");
	if (!fp)
		return NULL;
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	uint8_t *buf = malloc((size_t)sz);
	if (!buf) {
		fclose(fp);
		return NULL;
	}
	if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
		fclose(fp);
		free(buf);
		return NULL;
	}
	fclose(fp);
	*out_len = (size_t)sz;
	return buf;
}

/* Bus/VCA registries, keyed by their FMOD path (e.g. "bus:/music").
 * Celeste re-queries the same path every frame (volume sliders, pause
 * flags) and expects the state to persist, so we cache the nodes here
 * instead of allocating a throwaway object on each call. */
#define MAX_NODES	256

static BUS *bus_registry[MAX_NODES];
static int bus_registry_count = 0;
static VCA *vca_registry[MAX_NODES];
static int vca_registry_count = 0;

#define MAX_INSTANCES	65536
static EVENTINSTANCE *instance_registry[MAX_INSTANCES];
static int instance_registry_count = 0;

void register_instance(EVENTINSTANCE *instance)
{
	if (instance_registry_count >= MAX_INSTANCES)
		return;
	instance_registry[instance_registry_count++] = instance;
}

void free_instance(EVENTINSTANCE *instance)
{
	for (int i = 0; i < instance_registry_count; i++) {
		if (instance_registry[i] == instance) {
			instance_registry[i] =
				instance_registry[--instance_registry_count];
			break;
		}
	}
	free(instance);
}

BUS *find_or_create_bus(const char *path)
{
	for (int i = 0; i < bus_registry_count; i++) {
		if (bus_registry[i]->path &&
		    strcmp(bus_registry[i]->path, path) == 0)
			return bus_registry[i];
	}
	if (bus_registry_count >= MAX_NODES)
		return NULL;
	BUS *bus = malloc(sizeof(*bus));
	assert(bus != NULL);
	bus->path = path;
	bus->paused = false;
	bus->volume = 1.0f;
	bus->muted = false;
	bus_registry[bus_registry_count++] = bus;
	DPRINT(2, "new bus node: %s", path);
	return bus;
}

VCA *find_or_create_vca(const char *path)
{
	for (int i = 0; i < vca_registry_count; i++) {
		if (vca_registry[i]->path &&
		    strcmp(vca_registry[i]->path, path) == 0)
			return vca_registry[i];
	}
	if (vca_registry_count >= MAX_NODES)
		return NULL;
	VCA *vca = NewVca();
	vca->path = path;
	vca_registry[vca_registry_count++] = vca;
	DPRINT(2, "new vca node: %s", path);
	return vca;
}

/* Returns true if the event path (e.g. "event:/music/foo") falls under
 * the bus/VCA node path (e.g. "bus:/music").  The node path may have a
 * deeper prefix such as "bus:/music/stings". */
static bool path_under(const char *nodepath, const char *evpath)
{
	const char *n = nodepath;
	const char *e = evpath;

	/* Strip the scheme prefix ("bus:/", "vca:/", "event:/"). */
	const char *p = strchr(n, ':');
	if (p && p[1] == '/' && p[2] != '\0')
		n = p + 2;
	p = strchr(e, ':');
	if (p && p[1] == '/' && p[2] != '\0')
		e = p + 2;

	size_t nlen = strlen(n);
	if (nlen == 0)
		return false;
	if (strncmp(e, n, nlen) != 0)
		return false;
	/* Exact match, or e continues with a path separator. */
	return e[nlen] == '\0' || e[nlen] == '/';
}

/* FMOD stores the loop/oneshot flag in the event definition, which this
 * reimplementation does not have.  Celeste's looping content lives under a
 * handful of event-path prefixes (music, ambience/environment, classic
 * B-side music, new_content), while char/game/ui are one-shot SFX.  Use
 * that as the loop classifier so music and ambience repeat instead of
 * stopping after a single pass. */
static bool event_is_looping(const char *evpath)
{
	static const char *loop_prefixes[] = {
		"event:/music/", "event:/env/", "event:/classic/",
		"event:/new_content/music/", "event:/new_content/env/", NULL
	};
	if (!evpath)
		return false;
	for (int i = 0; loop_prefixes[i]; i++) {
		if (!strncmp(evpath, loop_prefixes[i], strlen(loop_prefixes[i])))
			return true;
	}
	return false;
}

/* Recompute OpenAL gain and pause state for every active StreamPlayer from
 * the current bus/VCA graph and the per-event volume/pitch.  Effective
 * gain = event_volume * (∏ matching bus volumes, 0 if any matching bus
 * is muted) * (∏ matching VCA volumes).  A source is paused if any
 * matching bus is paused. */
void apply_audio_state(void)
{
	for (int i = 0; i < sp_counter; i++) {
		StreamPlayer *sp = &StreamPlayerArr[i];
		if (sp->retired || sp->status == STOPPED)
			continue;

		const char *evpath = sp->fm_path;
		if (!evpath)
			continue;

		float gain = 1.0f;
		bool paused = false;

		/* Bus nodes: volume, mute, pause. */
		for (int b = 0; b < bus_registry_count; b++) {
			BUS *bus = bus_registry[b];
			if (!bus->path || !path_under(bus->path, evpath))
				continue;
			if (bus->muted) {
				gain = 0.0f;
			} else {
				gain *= bus->volume;
			}
			if (bus->paused)
				paused = true;
		}

		/* VCA nodes: volume only (no pause, no mute). */
		for (int v = 0; v < vca_registry_count; v++) {
			VCA *vca = vca_registry[v];
			if (!vca->path || !path_under(vca->path, evpath))
				continue;
			gain *= vca->volume;
		}

		/* Fold in the per-event volume (set via EventInstance_SetVolume). */
		gain *= sp->volume;

		alSourcef(sp->source, AL_GAIN, gain);
		if (paused) {
			if (sp->status != PAUSED) {
				alSourcePause(sp->source);
				sp->status = PAUSED;
			}
		} else {
			if (sp->status == PAUSED) {
				alSourcePlay(sp->source);
				sp->status = PLAYING;
			}
		}
	}
}

int bank_extract_fsb5(BANK *bank, const uint8_t *bank_data, size_t bank_size)
{
	const uint8_t *fsb_data;
	size_t fsb_size;
	fsb5_header header;
	fsb5_sample *samples = NULL;

	fsb_data = memmem(bank_data, bank_size, (const uint8_t *)"FSB5", 4);
	if (!fsb_data)
		return -1;
	fsb_size = bank_size - (fsb_data - bank_data);

	if (fsb5_parse(fsb_data, fsb_size, &header, &samples) < 0)
		return -1;

	fsb5_extract_samples(fsb_data, fsb_size, &header, samples);

	bank->sample_count = header.num_samples;
	bank->sample_names = reallocarray(NULL, header.num_samples, sizeof(char *));
	bank->sample_ogg_data = reallocarray(NULL, header.num_samples, sizeof(uint8_t *));
	bank->sample_ogg_len = reallocarray(NULL, header.num_samples, sizeof(size_t));
	for (uint32_t i = 0; i < header.num_samples; i++) {
		const char *sname = samples[i].name ? (const char *)samples[i].name : "sample";
		bank->sample_names[i] = strdup(sname);
		bank->sample_ogg_data[i] = samples[i].ogg_data;
		bank->sample_ogg_len[i] = samples[i].ogg_len;
		samples[i].ogg_data = NULL;
	}

	fsb5_free(&header, samples);
	return 0;
}

int bank_load_native(BANK *bank, const char *filename)
{
	FILE *fp;
	uint8_t *bank_data;
	size_t bank_size;
	char jo_path[MAXSTR];
	char shortname[MAXSTR];
	char *dot;
	json_value *jo;

	fp = fopen(filename, "rb");
	if (!fp)
		return -1;

	fseek(fp, 0, SEEK_END);
	bank_size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	bank_data = malloc(bank_size);
	if (!bank_data) {
		fclose(fp);
		return -1;
	}

	if (fread(bank_data, 1, bank_size, fp) != bank_size) {
		free(bank_data);
		fclose(fp);
		return -1;
	}
	fclose(fp);

	strlcpy(shortname, filename, sizeof(shortname));

	/* Native loading: parse the FSB5 blob directly and rebuild each sample
	 * as an OGG in memory (no disk I/O, no JSON manifest required).  The
	 * manifest is only consulted as a last-resort fallback if native loading
	 * fails. */
	if (bank_extract_fsb5(bank, bank_data, bank_size) == 0) {
		bank->native_loaded = true;

		/* Parse the bank's own FEV structure (bank GUID + event list)
		 * while the raw bytes are still available, so the loader
		 * identifies the bank and enumerates its events natively,
		 * without relying on a JSON manifest. */
		memset(&bank->fev, 0, sizeof(bank->fev));
		if (fev_parse(bank_data, bank_size, &bank->fev) == 0) {
			char gstr[37];
			DPRINT(1, "native FEV parse: %s guid=%s events=%u",
			       basename(shortname),
			       bank->fev.has_guid ?
					fev_guid_str(&bank->fev.guid, gstr, sizeof(gstr))
					: "(none)",
			       bank->fev.n_events);
		} else {
			DPRINT(1, "native FEV parse failed: %s",
			       basename(shortname));
		}

		free(bank_data);

		dot = strrchr(shortname, '.');
		if (dot)
			*dot = '\0';
		bank->path = strdup(basename(shortname));

		/* Resolve the bank's event->sample mapping from the embedded
		 * database so events can be looked up.  (The bank files do not
		 * carry the event->sample-name linkage; see bank_parse.h.) */
		memset(&bank->events, 0, sizeof(bank->events));
		if (events_db_find(basename(shortname), &bank->events)) {
			DPRINT(1, "native bank loaded: %s (%u events)",
			       filename, bank->events.count);
		} else {
			DPRINT(1, "native bank loaded: %s (no embedded events)",
			       filename);
		}
		return 0;
	}
	free(bank_data);

	/* Check for non-FSB5 banks (e.g. .strings.bank in FEV/RIFF format). */
	{
		FILE *fp2 = fopen(filename, "rb");
		if (fp2) {
			char magic[4];
		if (fread(magic, 1, 4, fp2) == 4 && memcmp(magic, "RIFF", 4) == 0) {
			fclose(fp2);

			/* Native STDT parsing: a .strings.bank carries the
			 * Event GUID -> path table.  Parse it and merge those
			 * mappings into the process-global registry so FMOD
			 * Studio GUID<->path queries (GetEventByID, LookupID,
			 * LookupPath, Bank_GetStringInfo) can be answered
			 * natively, without the events_db.  See docs/bank_parsing.md.
			 * The parsed strings live on the BANK for its lifetime. */
			uint8_t *sb = read_file_bytes(filename, &bank_size);
			if (sb) {
				memset(&bank->fev, 0, sizeof(bank->fev));
				if (fev_parse_strings(sb, bank_size, &bank->fev) == 0 &&
				    bank->fev.n_strings > 0) {
					fev_strmap_add_bank(&bank->fev);
					DPRINT(1, "strings bank parsed: %s (%u strings)",
					       basename(shortname),
					       bank->fev.n_strings);
				} else {
					DPRINT(1, "non-FSB5 bank (no STDT): %s",
					       basename(shortname));
				}
				free(sb);
			}
			bank->native_loaded = false;
			return 0;
		}
		fclose(fp2);
		}
	}

	/* Fallback: try the legacy JSON manifest next to the bank. */
	strlcpy(jo_path, filename, MAXSTR);
	dot = strrchr(jo_path, '.');
	if (dot)
		*dot = '\0';
	strlcat(jo_path, ".json", MAXSTR);

	jo = json_parse_file(jo_path);
	if (jo) {
		bank->jo = jo;
		bank->path = json_get_string(bank->jo, "path");
		bank->guid = json_get_string(bank->jo, "GUID");
		bank->native_loaded = false;
		return 0;
	}

	return -1;
}

void bank_free_native(BANK *bank)
{
	if (!bank)
		return;
	if (bank->native_loaded) {
		for (int i = 0; i < bank->sample_count; i++) {
			free(bank->sample_names[i]);
			free(bank->sample_ogg_data[i]);
		}
		free(bank->sample_names);
		free(bank->sample_ogg_data);
		free(bank->sample_ogg_len);
		events_db_free(&bank->events);
	}
	/* The fev struct is parsed for every bank flavour (sound banks via
	 * fev_parse, strings banks via fev_parse_strings) and owns the
	 * per-bank STDT strings, so always release it. */
	fev_bank_free(&bank->fev);
}

VCA *NewVca(void)
{
	VCA *vca;
	vca = malloc(sizeof(*vca));
	assert(vca != NULL);
	vca->path = NULL;
	vca->volume = 1.0;		// Start new VCAs at full volume
	vca->finalvolume = 1.0;
	return vca;
}

int FMOD_Studio_System_Create(SYSTEM **system, unsigned int headerversion)
{
	if (!init_done)
	{
		char *log_env;
		fprintf(stderr, "[%s] initializing OpenAL backend\n", PROJ);
		al_init();
		if ((log_env = getenv("FMOAD_LOGLEVEL")) != NULL)
		{
			loglevel = (unsigned int)atoi(log_env);
		}
		fprintf(stderr, "[%s] FMOAD_LOGLEVEL=%d\n", PROJ, loglevel);
		init_done = 1;
	}
	DPRINT(2, "headerversion %d", headerversion);
	*system = malloc(sizeof(**system));
	if (!*system)
		return -1;
	memset(*system, 0, sizeof(**system));
	(*system)->maxchannels = 32;
	return 0;
}

int FMOD_Studio_System_Initialize(SYSTEM *system,
	int maxchannels, FM_STUDIO_INITFLAGS studioflags,
	FM_INITFLAGS flags,
	void *extradriverdata)
{
	DPRINT(2, " STUB; maxchannels: %d", maxchannels);
	return 0;
}

int FMOD_Studio_System_SetListenerAttributes(SYSTEM *system,
	int listener,
	FM_3D_ATTRIBUTES *attributes)
{
	DPRINT(2, "listener %d", listener);
	return 0;
}

int FMOD_Studio_System_GetListenerAttributes(SYSTEM *system, int listener, FM_3D_ATTRIBUTES *attributes)
{
	DPRINT(2, "STUB; listener %d", listener);
	return 0;
}

int FMOD_Studio_System_Update(SYSTEM *system)
{
	for (int i = 0; i < sp_counter; i++)
	{
		DPRINT(4, "Update streamplayer %d", i);
		if (StreamPlayerArr[i].retired)
			continue;
		switch(StreamPlayerArr[i].status)
		{
			case PLAYING:
				if (!UpdatePlayer(&StreamPlayerArr[i])) {
					/* Playback finished (one-shot reached EOF and is
					 * not looping): mark stopped so the instance can
					 * be released and the source cleaned up. */
					StreamPlayerArr[i].status = STOPPED;
				}
				break;
			case PAUSED:
				break;
			case STOPPED:
				StopPlayer(&StreamPlayerArr[i]);
				if(StreamPlayerArr[i].released)
					DeletePlayer(&StreamPlayerArr[i]);
				break;
			default:
				// should not be reached
				fprintf(stderr, "Bad StreamPlayer status.\n");
		}
	}

	/* Free released EventInstances whose StreamPlayers have all been
	 * retired (the deferred-release lifecycle, see Release()). */
	for (int i = 0; i < instance_registry_count; i++) {
		EVENTINSTANCE *ei = instance_registry[i];
		if (!ei->released)
			continue;
		bool all_retired = true;
		if (ei->n_sp == 0)
			all_retired = true;	/* never had any players */
		for (int j = 0; j < ei->n_sp; j++) {
			int idx = ei->sp_idx[j];
			if (idx >= 0 && !StreamPlayerArr[idx].retired) {
				all_retired = false;
				break;
			}
		}
		if (all_retired)
			free_instance(ei);
	}
	return 0;
}

int FMOD_Studio_System_LoadBankFile(SYSTEM *system,
	const char *filename, FM_STUDIO_LOAD_BANK_FLAGS flags,
	BANK **bank)
{
	BANK *newbank = malloc(sizeof(BANK));
	if (!newbank)
		err(1, NULL);
	memset(newbank, 0, sizeof(*newbank));

	if (bank_load_native(newbank, filename) < 0) {
		free(newbank);
		return -1;
	}

	newbank->bankpath = filename;
	{
		const char *base = strrchr(filename, '/');
		newbank->name = base ? strdup(base + 1) : strdup(filename);
		if (base) {
			size_t len = (size_t)(base - filename);
			char *parent = malloc(len + 1);
			if (parent) {
				memcpy(parent, filename, len);
				parent[len] = '\0';
				newbank->parentdir = parent;
			} else {
				newbank->parentdir = strdup(filename);
			}
		} else {
			newbank->parentdir = strdup(".");
		}
	}

	DPRINT(2, "filename: %s, native: %d, path: %s, guid: %s",
	       filename, newbank->native_loaded,
	       newbank->path ? newbank->path : "(null)",
	       newbank->guid ? newbank->guid : "(null)");

	*bank = newbank;
	return 0;
}

int FMOD_Studio_System_GetVCA(SYSTEM *system, const char *path, VCA **vca)
{
	DPRINT(2, "path: %s", path);
	*vca = find_or_create_vca(path);
	return 0;
}

int FMOD_Studio_System_LoadBankMemory(SYSTEM *system,
	const char *buffer, int length, int mode,
	FM_STUDIO_LOAD_BANK_FLAGS flags,
	BANK **bank)
{
	BANK *newbank = malloc(sizeof(BANK));
	if (!newbank)
		err(1, NULL);
	memset(newbank, 0, sizeof(*newbank));

	const uint8_t *buf = (const uint8_t *)buffer;
	size_t buflen = (size_t)length;

	/* A .strings.bank (RIFF/STDT, no FSB5) is also loaded via this entry
	 * point.  Detect and parse its string table first so the GUID<->path
	 * registry is populated the same way as LoadBankFile.  See
	 * docs/bank_parsing.md. */
	memset(&newbank->fev, 0, sizeof(newbank->fev));
	if (buflen >= 4 && memcmp(buf, "RIFF", 4) == 0 &&
	    fev_parse_strings(buf, buflen, &newbank->fev) == 0 &&
	    newbank->fev.n_strings > 0) {
		fev_strmap_add_bank(&newbank->fev);
		newbank->native_loaded = false;
		newbank->bankpath = NULL;
		newbank->name = NULL;
		newbank->parentdir = NULL;
		*bank = newbank;
		return 0;
	}

	if (bank_extract_fsb5(newbank, buf, buflen) < 0) {
		fev_bank_free(&newbank->fev);
		free(newbank);
		return -1;
	}

	newbank->native_loaded = true;
	newbank->bankpath = NULL;
	newbank->name = NULL;
	newbank->parentdir = NULL;

	*bank = newbank;
	return 0;
}

int FMOD_Studio_VCA_SetVolume(VCA *vca, float volume)
{
	if (vca)
	{
		vca->volume = volume;
		vca->finalvolume = volume;
		apply_audio_state();
	}
	return 0;
}

int FMOD_Studio_VCA_GetVolume(VCA *vca, float *volume, float *finalvolume)
{
	DPRINT(1, "vca path: %s, volume: %.2f, finalvolume: %.2f", vca->path,
		vca->volume, vca->finalvolume);
	*volume = vca->volume;
	*finalvolume = vca->finalvolume;
	return 0;
}

int FMOD_Studio_System_GetEvent(SYSTEM *system, const char *path, EVENTDESCRIPTION **event)
{
	EVENTDESCRIPTION *newevent = malloc(sizeof(EVENTDESCRIPTION));
	if (!newevent)
		err(1, NULL);
	newevent->path = path;
	newevent->sound_idx = get_sound_idx((char *)path);
	/* Errors (sound_idx < 0) are handled in ..._CreateInstance */
	DPRINT(1, "path: %s, sound_idx: %d", newevent->path, newevent->sound_idx);
	*event = newevent;
	return 0;
}

int FMOD_Studio_EventDescription_LoadSampleData(EVENTDESCRIPTION *eventdescription)
{
	STUB();
}

int FMOD_Studio_EventDescription_CreateInstance(EVENTDESCRIPTION *eventdescription, EVENTINSTANCE **instance)
{
	int player_idx = -1; // -1 would be invalid
	int i;

	if (eventdescription->sound_idx < 0)
	{
		instance = NULL;
		return 0;	// don't mess; just keep going
	}
	EVENTINSTANCE *newinstance = malloc(sizeof(EVENTINSTANCE));
	memset(newinstance, 0, sizeof(*newinstance));
	newinstance->evd = eventdescription;
	newinstance->volume = 1.0f;
	newinstance->finalvolume = 1.0f;
	newinstance->paused = false;
	newinstance->released = false;
	newinstance->looping = event_is_looping(newinstance->evd->path);
	newinstance->pitch = 1.0f;
	memset(&newinstance->attributes, 0, sizeof(newinstance->attributes));
	int sound_num = newinstance->evd->sound_idx;
	DPRINT(1, "sp_counter: %d, evd->sound_idx: %d, path: %s, n_filepaths: %d", sp_counter, sound_num, sounds[sound_num].path, sounds[sound_num].n_filepaths);

	for (i = 0;
		i < sounds[newinstance->evd->sound_idx].n_filepaths;
		i++)
	{
		if (i >= MAX_INST_SP)
		{
			fprintf(stderr, "Error: exceding maximum number of StreamPlayers that can be associated with an EventInstance.\n");
			exit(1);
		}
		// check if a retired StreamPlayer can be reused
		for (int j = 0; j < sp_counter; j++)
		{
			if (StreamPlayerArr[j].retired)
			{
				player_idx = j;
				break;
			}
		}

		// no retired StreamPlayer found; therefore create a new one
		if (player_idx < 0)
			player_idx = sp_counter++;

		memset(&StreamPlayerArr[player_idx], 0, sizeof(StreamPlayer));
		StreamPlayer *np = NewPlayer();
		if (np == NULL) {
			/* Out of OpenAL voices; drop this instance gracefully
			 * instead of aborting the game. */
			DPRINT(1, "dropping voice, OpenAL limit reached");
			newinstance->sp_idx[i] = -1;
			continue;
		}
		StreamPlayerArr[player_idx] = *np;
		StreamPlayerArr[player_idx].looping = newinstance->looping;
		free(np);
		if (!OpenPlayerFile(&StreamPlayerArr[player_idx], sounds[newinstance->evd->sound_idx].mem_data[i], sounds[newinstance->evd->sound_idx].mem_len[i]))
		{
			fprintf(stderr, "ERROR with OpenPlayerFile; aborting\n");
			exit(1);	// TODO: return an error instead
		}
		StreamPlayerArr[player_idx].fm_path = sounds[newinstance->evd->sound_idx].path;
		newinstance->sp_idx[i] = player_idx;
	}
	newinstance->n_sp = i;
	*instance = newinstance;
	register_instance(newinstance);
	return 0;
}


int FMOD_Studio_EventDescription_Is3D(EVENTDESCRIPTION *eventdescription, bool *is3D)
{
	if (eventdescription && is3D)
	{
		*is3D = false;
	}
	return 0;
}

/* Returns true if the instance is still registered and alive. */
static bool instance_is_alive(EVENTINSTANCE *instance)
{
	for (int i = 0; i < instance_registry_count; i++) {
		if (instance_registry[i] == instance)
			return true;
	}
	return false;
}

int FMOD_Studio_EventInstance_Start(EVENTINSTANCE *eventinstance)
{
	int i;

	if (!eventinstance || !instance_is_alive(eventinstance))
	{
		return 0;
	}
	for (i = 0; i < eventinstance->n_sp; i++)
	{
		int idx = eventinstance->sp_idx[i];
		if (idx < 0 || idx >= (int)sp_counter)
			continue;
		if (StreamPlayerArr[idx].retired)
			continue;
		DPRINT(1, "sound_idx: %d, sp_idx: %d, path: %s", eventinstance->evd->sound_idx, idx, sounds[eventinstance->evd->sound_idx].path);
		if(!StartPlayer(&StreamPlayerArr[idx]))
		{
			fprintf(stderr, "ERROR in StartPlayer\n");
			exit(1);
		}
		StreamPlayerArr[idx].status = PLAYING;
		StreamPlayerArr[idx].volume = eventinstance->volume;
	}
	apply_audio_state();
	return 0;
}

int FMOD_Studio_System_GetBus(SYSTEM *system, const char *path, BUS **bus)
{
	DPRINT(1, "path %s", path);
	*bus = find_or_create_bus(path);
	return 0;
}

int FMOD_Studio_Bus_SetPaused(BUS *bus, bool paused)
{
	// true = paused, false = unpaused
	DPRINT(1, "paused %d, path %s", (int)paused, bus ? bus->path : "(null)");
	if (bus) {
		bus->paused = paused;
		apply_audio_state();
	}
	return 0;
}

int FMOD_Studio_Bus_GetPaused(BUS *bus, bool *paused)
{
	if (bus && paused)
		*paused = bus->paused;
	return 0;
}

int FMOD_Studio_Bus_SetMute(BUS *bus, bool mute)
{
	DPRINT(1, "mute %d, path %s", (int)mute, bus ? bus->path : "(null)");
	if (bus) {
		bus->muted = mute;
		apply_audio_state();
	}
	return 0;
}

int FMOD_Studio_Bus_GetMute(BUS *bus, bool *mute)
{
	if (bus && mute)
		*mute = bus->muted;
	return 0;
}

int FMOD_Studio_Bus_SetVolume(BUS *bus, float volume)
{
	DPRINT(1, "volume %.2f, path %s", volume, bus ? bus->path : "(null)");
	if (bus) {
		bus->volume = volume;
		apply_audio_state();
	}
	return 0;
}

int FMOD_Studio_Bus_GetVolume(BUS *bus, float *volume, float *finalvolume)
{
	if (bus) {
		if (volume)
			*volume = bus->volume;
		if (finalvolume)
			*finalvolume = bus->muted ? 0.0f : bus->volume;
	}
	return 0;
}

int FMOD_Studio_Bus_StopAllEvents(BUS *bus, FM_STOP_MODE mode)
{
	DPRINT(1, "mode %d, path %s", (int)mode, bus ? bus->path : "(null)");
	if (!bus || !bus->path)
		return 0;
	for (int i = 0; i < sp_counter; i++) {
		StreamPlayer *sp = &StreamPlayerArr[i];
		if (sp->retired || sp->status == STOPPED)
			continue;
		if (sp->fm_path && path_under(bus->path, sp->fm_path)) {
			sp->status = STOPPED;
			sp->released = (mode == STOP_IMMEDIATE);
		}
	}
	return 0;
}

int FMOD_Studio_EventInstance_GetDescription(EVENTINSTANCE *eventinstance, EVENTDESCRIPTION **description)
{
	*description = eventinstance->evd;
	return 0;
}

int FMOD_Studio_EventDescription_GetPath(EVENTDESCRIPTION *eventdescription, char *path, int size, int *retrieved)
{
	if (eventdescription && path && retrieved)
	{
		strlcpy(path, eventdescription->path, size);
		*retrieved = strnlen(path, size - 1) + 1;
		DPRINT(1, "path %s, buffer size %d, retrieved %d", path, size, *retrieved);
	}
	return 0;
}

int FMOD_Studio_Bank_LoadSampleData(BANK *bank)
{
	if (bank->native_loaded) {
		/* Key each sound object by its FMOD event path and point it at
		 * the rebuilt OGG in memory.  The event->sample mapping comes
		 * from the embedded events DB. */
		if (bank->events.count > 0) {
			for (uint32_t i = 0; i < bank->events.count; i++) {
				const char *evpath = bank->events.entries[i].path;
				const char *file = bank->events.entries[i].file;
				int sample_idx = -1;
				for (int j = 0; j < bank->sample_count; j++) {
					if (!strcmp(bank->sample_names[j], file)) {
						sample_idx = j;
						break;
					}
				}
				if (sample_idx < 0) {
					DPRINT(1, "native event sample not found: %s", file);
					continue;
				}

				sounds[sound_counter] = *NewSoundObject();
				sounds[sound_counter].path = evpath;
				sounds[sound_counter].n_filepaths = 1;
				sounds[sound_counter].mem_data =
					reallocarray(NULL, 1, sizeof(uint8_t *));
				sounds[sound_counter].mem_len =
					reallocarray(NULL, 1, sizeof(size_t));
				sounds[sound_counter].mem_data[0] =
					bank->sample_ogg_data[sample_idx];
				sounds[sound_counter].mem_len[0] =
					bank->sample_ogg_len[sample_idx];
				DPRINT(1, "native event %s -> sample %s (%zu bytes)",
				       evpath, file, sounds[sound_counter].mem_len[0]);
				sound_counter++;
			}
		} else {
			/* No embedded event mapping (e.g. a strings/sample-only
			 * bank): expose samples by their FSB5 name as a fallback. */
			for (int i = 0; i < bank->sample_count; i++) {
				sounds[sound_counter] = *NewSoundObject();
				sounds[sound_counter].path = bank->sample_names[i];
				sounds[sound_counter].n_filepaths = 1;
				sounds[sound_counter].mem_data =
					reallocarray(NULL, 1, sizeof(uint8_t *));
				sounds[sound_counter].mem_len =
					reallocarray(NULL, 1, sizeof(size_t));
				sounds[sound_counter].mem_data[0] =
					bank->sample_ogg_data[i];
				sounds[sound_counter].mem_len[0] =
					bank->sample_ogg_len[i];
				sound_counter++;
			}
		}
		DPRINT(2, "native sound_counter: %d", sound_counter);
		return 0;
	}

	if (!bank->jo) {
		DPRINT(1, "no JSON manifest and no native data");
		return 0;
	}

	const json_value *events = json_get(bank->jo, "events");
	size_t n_events = json_array_length(events);
	const json_value *event;
	for (int i = 0; i < n_events; i++)
	{
		int j;
		const json_value *files;
		const json_value *file;
		int num_files;
		char *filename = reallocarray(NULL, MAXSTR, sizeof(char));
		char *path = reallocarray(NULL, MAXSTR, sizeof(char));
		event = json_array_get(events, i);
		files = json_get(event, "files");
		path = (char *)json_get_string(event, "path");
		num_files = json_array_length(files);
		uint8_t **mem_data_buf = reallocarray(NULL, num_files, sizeof(uint8_t *));
		size_t *mem_len_buf = reallocarray(NULL, num_files, sizeof(size_t));
		DPRINT(1, "%s: %d", path, num_files);
		sounds[sound_counter] = *NewSoundObject();
		for (j = 0;
			j < num_files;
			j++)
		{
			filename[0] = '\0';	// empty the string array
			file = json_array_get(files, j);
			filename = (char *)json_get_string(file, "filename");
			DPRINT(1, "ogg_path: %s", filename);
			mem_data_buf[j] = NULL;
			mem_len_buf[j] = 0;
			sounds[sound_counter].path = path;
		}
		sounds[sound_counter].n_filepaths = j;
		sounds[sound_counter].mem_data = mem_data_buf;
		sounds[sound_counter].mem_len = mem_len_buf;
		sound_counter++;
	}
	DPRINT(2, "sound_counter: %d", sound_counter);
	return 0;
}

int FMOD_Studio_EventInstance_SetVolume(EVENTINSTANCE *eventinstance, float volume)
{
	if (!eventinstance || !instance_is_alive(eventinstance))
	{
		return 0;
	}
	eventinstance->volume = volume;
	eventinstance->finalvolume = volume;
	for (int i = 0; i < eventinstance->n_sp; i++)
	{
		int idx = eventinstance->sp_idx[i];
		if (idx < 0 || idx >= (int)sp_counter)
			continue;
		if (StreamPlayerArr[idx].retired)
			continue;
		StreamPlayerArr[idx].volume = volume;
	}
	apply_audio_state();
	return 0;
}

int FMOD_Studio_System_GetLowLevelSystem(SYSTEM *system, void **lowLevelSystem)
{
	STUB();
}

int FMOD_Studio_EventInstance_Set3DAttributes(EVENTINSTANCE *eventinstance, FM_3D_ATTRIBUTES *attributes)
{
	if (eventinstance && attributes)
	{
		eventinstance->attributes = *attributes;
		for (int i = 0; i < eventinstance->n_sp; i++)
		{
			if (eventinstance->sp_idx[i] < 0)
				continue;
			alSource3f(StreamPlayerArr[eventinstance->sp_idx[i]].source,
				AL_POSITION, attributes->position.x,
				attributes->position.y, attributes->position.z);
			alSource3f(StreamPlayerArr[eventinstance->sp_idx[i]].source,
				AL_VELOCITY, attributes->velocity.x,
				attributes->velocity.y, attributes->velocity.z);
			ALfloat orientation[6];
			orientation[0] = attributes->forward.x;
			orientation[1] = attributes->forward.y;
			orientation[2] = attributes->forward.z;
			orientation[3] = attributes->up.x;
			orientation[4] = attributes->up.y;
			orientation[5] = attributes->up.z;
			alSourcefv(StreamPlayerArr[eventinstance->sp_idx[i]].source,
				AL_ORIENTATION, orientation);
		}
	}
	return 0;
}

int FMOD_Studio_EventInstance_Release(EVENTINSTANCE *eventinstance)
{
	if (!eventinstance || !instance_is_alive(eventinstance))
	{
		return 0;
	}
	eventinstance->released = true;
	for (int i = 0; i < eventinstance->n_sp; i++)
	{
		int idx = eventinstance->sp_idx[i];
		if (idx < 0 || idx >= (int)sp_counter)
			continue;
		StreamPlayerArr[idx].released = true;
	}
	return 0;
}

int FMOD_Studio_EventInstance_GetVolume(EVENTINSTANCE *eventinstance, float *volume, float *finalvolume)
{
	if (eventinstance)
	{
		if (volume)
			*volume = eventinstance->volume;
		if (finalvolume)
			*finalvolume = eventinstance->finalvolume;
	}
	return 0;
}

int FMOD_Studio_EventInstance_Stop(EVENTINSTANCE *eventinstance, FM_STOP_MODE mode)
{
	if (!eventinstance || !instance_is_alive(eventinstance))
	{
		return 0;
	}
	for (int i = 0; i < eventinstance->n_sp; i++)
	{
		int idx = eventinstance->sp_idx[i];
		if (idx < 0 || idx >= (int)sp_counter)
			continue;
		if (StreamPlayerArr[idx].retired)
			continue;
		DPRINT(1, "stop mode: %d, on sp_idx: %d, path: %s", (int)mode, idx, eventinstance->evd->path);
		StreamPlayerArr[idx].status = STOPPED;
		StopPlayer(&StreamPlayerArr[idx]);
	}
	return 0;
}

int FMOD_Studio_EventInstance_Get3DAttributes(EVENTINSTANCE *eventinstance, FM_3D_ATTRIBUTES *attributes)
{
	if (eventinstance && attributes)
	{
		*attributes = eventinstance->attributes;
	}
	return 0;
}

int FMOD_Studio_System_Release(SYSTEM *system)
{
	DPRINT(2, "release");
	for (int i = 0; i < instance_registry_count; i++)
		free(instance_registry[i]);
	instance_registry_count = 0;
	for (int i = 0; i < bus_registry_count; i++)
		free(bus_registry[i]);
	bus_registry_count = 0;
	for (int i = 0; i < vca_registry_count; i++)
		free(vca_registry[i]);
	vca_registry_count = 0;
	if (system)
		free(system);
	al_shutdown();
	init_done = false;
	return 0;
}

int FMOD_Studio_EventInstance_SetParameterValue(EVENTINSTANCE *eventinstance, char *name, float value)
{
	DPRINT(4, "STUB; name: %s, value: %.2f", name, value);
	return 0;
}

int FMOD_Studio_EventDescription_IsOneshot(EVENTDESCRIPTION *eventdescription, bool *oneshot)
{
	if (eventdescription && oneshot)
	{
		/* Looping events (music/ambience) are not one-shots, so the
		 * game's SoundSource does not auto-release them while they
		 * keep playing. */
		*oneshot = !event_is_looping(eventdescription->path);
	}
	return 0;
}

int FMOD_Studio_EventInstance_SetPaused(EVENTINSTANCE *eventinstance, bool paused)
{
	if (!eventinstance || !instance_is_alive(eventinstance))
	{
		return 0;
	}
	eventinstance->paused = paused;
	for (int i = 0; i < eventinstance->n_sp; i++)
	{
		int idx = eventinstance->sp_idx[i];
		if (idx < 0 || idx >= (int)sp_counter)
			continue;
		if (StreamPlayerArr[idx].retired)
			continue;
		StreamPlayerArr[idx].status = paused ? PAUSED : PLAYING;
	}
	return 0;
}

int FMOD_Studio_EventInstance_TriggerCue(EVENTINSTANCE *eventinstance)
{
	STUB();
}

int FMOD_Studio_EventInstance_GetPaused(EVENTINSTANCE *eventinstance, bool *paused)
{
	if (eventinstance && paused)
	{
		*paused = eventinstance->paused;
	}
	return 0;
}

int FMOD_Studio_EventInstance_GetPlaybackState(EVENTINSTANCE *eventinstance, int *state)
{
	if (eventinstance && state)
	{
		if (eventinstance->paused)
			*state = PLAYBACK_SUSTAINING;
		else if (eventinstance->n_sp > 0 && eventinstance->sp_idx[0] >= 0 &&
		    StreamPlayerArr[eventinstance->sp_idx[0]].status == PLAYING)
			*state = PLAYBACK_PLAYING;
		else
			*state = PLAYBACK_STOPPED;
	}
	return 0;
}

int FMOD_Studio_System_LoadBankCustom(SYSTEM * studiosystem, void * info, void * flags, void * bank)
{
	STUB();
}

int FMOD_Studio_System_UnloadAll(SYSTEM * studiosystem)
{
	STUB();
}

int FMOD_Studio_System_GetBank(SYSTEM * studiosystem, char * path, void * bank)
{
	STUB();
}

int FMOD_Studio_System_GetBankByID(SYSTEM * studiosystem, void * guid, void * bank)
{
	STUB();
}

int FMOD_Studio_System_GetBankCount(SYSTEM * studiosystem, int * count)
{
	STUB();
}

int FMOD_Studio_System_GetBankList(SYSTEM * studiosystem, void * array, int capacity, int * count)
{
	STUB();
}

int FMOD_Studio_System_GetBusByID(SYSTEM * studiosystem, void * guid, void * bus)
{
	STUB();
}

int FMOD_Studio_System_GetEventByID(SYSTEM * studiosystem, void * guid, void * description)
{
	struct fev_guid g;
	memcpy(g.data, guid, 16);
	const char *path = fev_strmap_lookup_path(&g);
	if (!path) {
		DPRINT(1, "GetEventByID: unknown GUID");
		*(EVENTDESCRIPTION **)description = NULL;
		return 0;
	}
	/* Delegate to the path-based lookup so the description is built the
	 * same way (and resolves its sound_idx from the same registry). */
	return FMOD_Studio_System_GetEvent(studiosystem, path,
	                                   (EVENTDESCRIPTION **)description);
}

int FMOD_Studio_System_GetVCAByID(SYSTEM * studiosystem, void * guid, void * vca)
{
	STUB();
}

int FMOD_Studio_System_LookupID(SYSTEM * studiosystem, char * path, void * guid)
{
	/* Native path -> GUID resolution from the global STDT registry
	 * (built from .strings.bank files).  The registry is keyed by the
	 * canonical GUID string, so we must find the entry whose path
	 * matches and copy back its raw 16-byte GUID.  See docs/bank_parsing.md. */
	struct fev_strmap *sm = fev_strmap_get();
	for (uint32_t i = 0; i < sm->count; i++) {
		if (sm->entries[i].path && strcmp(sm->entries[i].path, path) == 0) {
			memcpy(guid, sm->entries[i].guid.data, 16);
			return 0;
		}
	}
	DPRINT(1, "LookupID: unknown path %s", path);
	return 0;
}

int FMOD_Studio_System_LookupPath(SYSTEM * studiosystem, void * guid, void * path, int size, int * retrieved)
{
	struct fev_guid g;
	memcpy(g.data, guid, 16);
	const char *p = fev_strmap_lookup_path(&g);
	if (!p) {
		DPRINT(1, "LookupPath: unknown GUID");
		return 0;
	}
	strlcpy((char *)path, p, size);
	if (retrieved)
		*retrieved = strnlen(p, size - 1) + 1;
	return 0;
}

int FMOD_Studio_System_GetSoundInfo(SYSTEM * studiosystem, char * key, void * info)
{
	STUB();
}

int FMOD_Studio_System_GetUserData(SYSTEM * studiosystem, void * userdata)
{
	STUB();
}

int FMOD_Studio_System_SetUserData(SYSTEM * studiosystem, void * userdata)
{
	STUB();
}

int FMOD_Studio_System_GetAdvancedSettings(SYSTEM * studiosystem, void * settings)
{
	STUB();
}

int FMOD_Studio_System_SetAdvancedSettings(SYSTEM * studiosystem, void * settings)
{
	STUB();
}

int FMOD_Studio_System_GetBufferUsage(SYSTEM * studiosystem, void * usage)
{
	STUB();
}

int FMOD_Studio_System_ResetBufferUsage(SYSTEM * studiosystem)
{
	STUB();
}

int FMOD_Studio_System_GetCPUUsage(SYSTEM * studiosystem, void * usage)
{
	STUB();
}

int FMOD_Studio_System_GetNumListeners(SYSTEM * studiosystem, int * numlisteners)
{
	STUB();
}

int FMOD_Studio_System_GetListenerWeight(SYSTEM * studiosystem, int listener, float * weight)
{
	STUB();
}

int FMOD_Studio_System_SetNumListeners(SYSTEM * studiosystem, int numlisteners)
{
	STUB();
}

int FMOD_Studio_System_SetListenerWeight(SYSTEM * studiosystem, int listener, float weight)
{
	STUB();
}

int FMOD_Studio_System_FlushCommands(SYSTEM * studiosystem)
{
	STUB();
}

int FMOD_Studio_System_FlushSampleLoading(SYSTEM * studiosystem)
{
	STUB();
}

int FMOD_Studio_System_SetCallback(SYSTEM * studiosystem, void * callback, void * callbackmask)
{
	STUB();
}

int FMOD_Studio_System_StartCommandCapture(SYSTEM * studiosystem, char * path, void * flags)
{
	STUB();
}

int FMOD_Studio_System_StopCommandCapture(SYSTEM * studiosystem)
{
	STUB();
}

int FMOD_Studio_System_IsValid(SYSTEM * studiosystem)
{
	STUB();
}

int FMOD_Studio_Bank_Unload(BANK * bank)
{
	STUB();
}

int FMOD_Studio_Bank_UnloadSampleData(BANK * bank)
{
	STUB();
}

int FMOD_Studio_Bank_IsValid(BANK * bank)
{
	STUB();
}

int FMOD_Studio_Bank_GetBusCount(BANK * bank, int * count)
{
	STUB();
}

int FMOD_Studio_Bank_GetBusList(BANK * bank, void * array, int capacity, int * count)
{
	STUB();
}

int FMOD_Studio_Bank_GetEventCount(BANK * bank, int * count)
{
	STUB();
}

int FMOD_Studio_Bank_GetEventList(BANK * bank, void * array, int capacity, int * count)
{
	STUB();
}

int FMOD_Studio_Bank_GetID(BANK * bank, void * id)
{
	STUB();
}

int FMOD_Studio_Bank_GetLoadingState(BANK * bank, void * state)
{
	STUB();
}

int FMOD_Studio_Bank_GetPath(BANK * bank, void * path, int size, int * retrieved)
{
	STUB();
}

int FMOD_Studio_Bank_GetSampleLoadingState(BANK * bank, void * state)
{
	STUB();
}

int FMOD_Studio_Bank_GetStringCount(BANK * bank, int * count)
{
	if (!bank || !count)
		return 0;
	*count = (int)bank->fev.n_strings;
	return 0;
}

int FMOD_Studio_Bank_GetStringInfo(BANK * bank, int index, void * id, void * path, int size, int * retrieved)
{
	if (!bank || index < 0 || (uint32_t)index >= bank->fev.n_strings)
		return 0;
	if (id)
		memcpy(id, bank->fev.strings[index].guid.data, 16);
	if (path && size > 0) {
		const char *p = bank->fev.strings[index].path ?
			bank->fev.strings[index].path : "";
		strlcpy((char *)path, p, size);
		if (retrieved)
			*retrieved = strnlen(p, size - 1) + 1;
	}
	return 0;
}

int FMOD_Studio_Bank_GetUserData(BANK * studiosystem, void * userdata)
{
	STUB();
}

int FMOD_Studio_Bank_GetVCACount(BANK * bank, int * count)
{
	STUB();
}

int FMOD_Studio_Bank_GetVCAList(BANK * bank, void * array, int capacity, int * count)
{
	STUB();
}

int FMOD_Studio_Bank_SetUserData(BANK * studiosystem, void * userdata)
{
	STUB();
}

int FMOD_Studio_Bus_GetID(BUS * bus, void * id)
{
	STUB();
}

int FMOD_Studio_Bus_GetPath(BUS * bus, void * path, int size, int * retrieved)
{
	STUB();
}

int FMOD_Studio_Bus_GetChannelGroup(BUS * bus, void * group)
{
	STUB();
}

int FMOD_Studio_Bus_LockChannelGroup(BUS * bus)
{
	STUB();
}

int FMOD_Studio_Bus_UnlockChannelGroup(BUS * bus)
{
	STUB();
}

int FMOD_Studio_Bus_IsValid(BUS * bus)
{
	STUB();
}

int FMOD_Studio_EventDescription_GetID(EVENTDESCRIPTION * eventdescription, void * id)
{
	/* Resolve the event's GUID natively from the global STDT registry.
	 * The event description only stores its path, so we look that path
	 * up and copy back the 16-byte GUID. */
	if (!eventdescription || !id)
		return 0;
	struct fev_strmap *sm = fev_strmap_get();
	for (uint32_t i = 0; i < sm->count; i++) {
		if (sm->entries[i].path &&
		    strcmp(sm->entries[i].path, eventdescription->path) == 0) {
			memcpy(id, sm->entries[i].guid.data, 16);
			return 0;
		}
	}
	DPRINT(1, "GetID: unknown path %s", eventdescription->path);
	return 0;
}

int FMOD_Studio_EventDescription_GetInstanceCount(EVENTDESCRIPTION * eventdescription, int * count)
{
	STUB();
}

int FMOD_Studio_EventDescription_GetInstanceList(EVENTDESCRIPTION * eventdescription, void * array, int capacity, int * count)
{
	STUB();
}

int FMOD_Studio_EventDescription_GetLength(EVENTDESCRIPTION * eventdescription, int * length)
{
	STUB();
}

int FMOD_Studio_EventDescription_GetMaximumDistance(EVENTDESCRIPTION * eventdescription, float * distance)
{
	STUB();
}

int FMOD_Studio_EventDescription_GetMinimumDistance(EVENTDESCRIPTION * eventdescription, float * distance)
{
	STUB();
}

int FMOD_Studio_EventDescription_GetParameterByIndex(EVENTDESCRIPTION * eventdescription, int index, void * parameter)
{
	STUB();
}

int FMOD_Studio_EventDescription_GetParameterCount(EVENTDESCRIPTION * eventdescription, int * count)
{
	STUB();
}

int FMOD_Studio_EventDescription_GetParameter(EVENTDESCRIPTION * eventdescription, char * name, void * parameter)
{
	STUB();
}

int FMOD_Studio_EventDescription_GetSampleLoadingState(EVENTDESCRIPTION * eventdescription, void * state)
{
	STUB();
}

int FMOD_Studio_EventDescription_GetSoundSize(EVENTDESCRIPTION * eventdescription, float * size)
{
	STUB();
}

int FMOD_Studio_EventDescription_GetUserData(EVENTDESCRIPTION * eventdescription, void * userdata)
{
	STUB();
}

int FMOD_Studio_EventDescription_GetUserPropertyByIndex(EVENTDESCRIPTION * eventdescription, int index, void * property)
{
	STUB();
}

int FMOD_Studio_EventDescription_GetUserPropertyCount(EVENTDESCRIPTION * eventdescription, int * count)
{
	STUB();
}

int FMOD_Studio_EventDescription_GetUserProperty(EVENTDESCRIPTION * eventdescription, char * name, void * property)
{
	STUB();
}

int FMOD_Studio_EventDescription_HasCue(EVENTDESCRIPTION * eventdescription, bool * cue)
{
	STUB();
}

int FMOD_Studio_EventDescription_IsSnapshot(EVENTDESCRIPTION * eventdescription, bool * snapshot)
{
	STUB();
}

int FMOD_Studio_EventDescription_IsStream(EVENTDESCRIPTION * eventdescription, bool * isStream)
{
	STUB();
}

int FMOD_Studio_EventDescription_IsValid(EVENTDESCRIPTION * eventdescription)
{
	STUB();
}

int FMOD_Studio_EventDescription_ReleaseAllInstances(EVENTDESCRIPTION * eventdescription)
{
	STUB();
}

int FMOD_Studio_EventDescription_SetCallback(EVENTDESCRIPTION * eventdescription, void * callback, void * callbackmask)
{
	STUB();
}

int FMOD_Studio_EventDescription_SetUserData(EVENTDESCRIPTION * eventdescription, void * userdata)
{
	STUB();
}

int FMOD_Studio_EventDescription_UnloadSampleData(EVENTDESCRIPTION * eventdescription)
{
	STUB();
}

int FMOD_Studio_EventInstance_GetPitch(EVENTINSTANCE * _event, float * pitch, float * finalpitch)
{
	if (_event) {
		if (pitch)
			*pitch = _event->pitch;
		if (finalpitch)
			*finalpitch = _event->pitch;
	}
	return 0;
}

int FMOD_Studio_EventInstance_SetPitch(EVENTINSTANCE * _event, float pitch)
{
	if (!_event || !instance_is_alive(_event))
	{
		return 0;
	}
	_event->pitch = pitch;
	for (int i = 0; i < _event->n_sp; i++) {
		int idx = _event->sp_idx[i];
		if (idx < 0 || idx >= (int)sp_counter)
			continue;
		if (StreamPlayerArr[idx].retired)
			continue;
		alSourcef(StreamPlayerArr[idx].source,
			AL_PITCH, pitch);
	}
	return 0;
}

int FMOD_Studio_EventInstance_GetListenerMask(EVENTINSTANCE * _event, unsigned int * mask)
{
	STUB();
}

int FMOD_Studio_EventInstance_SetListenerMask(EVENTINSTANCE * _event, unsigned int mask)
{
	STUB();
}

int FMOD_Studio_EventInstance_GetProperty(EVENTINSTANCE * _event, void * index, float * value)
{
	STUB();
}

int FMOD_Studio_EventInstance_SetProperty(EVENTINSTANCE * _event, void * index, float value)
{
	STUB();
}

int FMOD_Studio_EventInstance_GetReverbLevel(EVENTINSTANCE * _event, int index, float * level)
{
	STUB();
}

int FMOD_Studio_EventInstance_SetReverbLevel(EVENTINSTANCE * _event, int index, float level)
{
	STUB();
}

int FMOD_Studio_EventInstance_GetTimelinePosition(EVENTINSTANCE * _event, int * position)
{
	STUB();
}

int FMOD_Studio_EventInstance_SetTimelinePosition(EVENTINSTANCE * _event, int position)
{
	STUB();
}

int FMOD_Studio_EventInstance_GetUserData(EVENTINSTANCE * _event, void * userdata)
{
	STUB();
}

int FMOD_Studio_EventInstance_SetUserData(EVENTINSTANCE * _event, void * userdata)
{
	STUB();
}

int FMOD_Studio_EventInstance_IsVirtual(EVENTINSTANCE * _event, bool * virtualstate)
{
	STUB();
}

int FMOD_Studio_EventInstance_GetParameter(EVENTINSTANCE * _event, char * name, void * parameter)
{
	STUB();
}

int FMOD_Studio_EventInstance_GetParameterByIndex(EVENTINSTANCE * _event, int index, void * parameter)
{
	STUB();
}

int FMOD_Studio_EventInstance_GetParameterCount(EVENTINSTANCE * _event, int * count)
{
	STUB();
}

int FMOD_Studio_EventInstance_GetParameterValue(EVENTINSTANCE * _event, char * name, float * value, float * finalvalue)
{
	STUB();
}

int FMOD_Studio_EventInstance_GetParameterValueByIndex(EVENTINSTANCE * _event, int index, float * value, float * finalvalue)
{
	STUB();
}

int FMOD_Studio_EventInstance_SetParameterValueByIndex(EVENTINSTANCE * _event, int index, float value)
{
	STUB();
}

int FMOD_Studio_EventInstance_SetParameterValuesByIndices(EVENTINSTANCE * _event, void * indices, void * values, int count)
{
	STUB();
}

int FMOD_Studio_EventInstance_SetCallback(EVENTINSTANCE * _event, void * callback, void * callbackmask)
{
	STUB();
}

int FMOD_Studio_EventInstance_GetChannelGroup(EVENTINSTANCE * _event, void * group)
{
	STUB();
}

int FMOD_Studio_EventInstance_IsValid(EVENTINSTANCE *eventinstance)
{
	return eventinstance != NULL;
}

int FMOD_Studio_VCA_GetID(VCA * vca, void * id)
{
	STUB();
}

int FMOD_Studio_VCA_GetPath(VCA * vca, void * path, int size, int * retrieved)
{
	STUB();
}

int FMOD_Studio_VCA_IsValid(VCA * vca)
{
	STUB();
}

int FMOD_Studio_ParameterInstance_GetDescription(void * parameter, void * description)
{
	STUB();
}

int FMOD_Studio_ParameterInstance_GetValue(void * parameter, float * value)
{
	STUB();
}

int FMOD_Studio_ParameterInstance_SetValue(void * parameter, float value)
{
	STUB();
}

int FMOD_Studio_ParameterInstance_IsValid(void * parameter)
{
	STUB();
}

int FMOD_Studio_CommandReplay_GetCommandAtTime(void * replay, float time, int * commandIndex)
{
	STUB();
}

int FMOD_Studio_CommandReplay_GetCommandCount(void * replay, int * count)
{
	STUB();
}

int FMOD_Studio_CommandReplay_GetCommandInfo(void * replay, int commandIndex, void * info)
{
	STUB();
}

int FMOD_Studio_CommandReplay_GetCommandString(void * replay, int commandIndex, void * description, int capacity)
{
	STUB();
}

int FMOD_Studio_CommandReplay_GetCurrentCommand(void * replay, int * commandIndex, float * currentTime)
{
	STUB();
}

int FMOD_Studio_CommandReplay_GetLength(void * replay, float * totalTime)
{
	STUB();
}

int FMOD_Studio_CommandReplay_GetPaused(void * replay, bool * paused)
{
	STUB();
}

int FMOD_Studio_CommandReplay_GetPlaybackState(void * replay, void * state)
{
	STUB();
}

int FMOD_Studio_CommandReplay_GetSystem(void * replay, void * system)
{
	STUB();
}

int FMOD_Studio_CommandReplay_GetUserData(void * replay, void * userdata)
{
	STUB();
}

int FMOD_Studio_CommandReplay_IsValid(void * replay)
{
	STUB();
}

int FMOD_Studio_CommandReplay_Release(void * replay)
{
	STUB();
}

int FMOD_Studio_CommandReplay_SeekToCommand(void * replay, int commandIndex)
{
	STUB();
}

int FMOD_Studio_CommandReplay_SeekToTime(void * replay, float time)
{
	STUB();
}

int FMOD_Studio_CommandReplay_SetBankPath(void * replay, char * bankPath)
{
	STUB();
}

int FMOD_Studio_CommandReplay_SetCreateInstanceCallback(void * replay, void * callback)
{
	STUB();
}

int FMOD_Studio_CommandReplay_SetFrameCallback(void * replay, void * callback)
{
	STUB();
}

int FMOD_Studio_CommandReplay_SetLoadBankCallback(void * replay, void * callback)
{
	STUB();
}

int FMOD_Studio_CommandReplay_SetPaused(void * replay, bool paused)
{
	STUB();
}

int FMOD_Studio_CommandReplay_SetUserData(void * replay, void * userdata)
{
	STUB();
}

int FMOD_Studio_CommandReplay_Start(void * replay)
{
	STUB();
}

int FMOD_Studio_CommandReplay_Stop(void * replay)
{
	STUB();
}

int FMOD_Studio_ParseID(char * idString, void * guid)
{
	STUB();
}
