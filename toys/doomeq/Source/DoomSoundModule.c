/*
 * DOOM's sound and music modules for the plug-in.
 *
 * Sound effects are DMX lumps (8-bit unsigned PCM, usually 11025 Hz); the lump stays
 * cached as PU_STATIC so the audio thread can read it in place. Music is MIDI (Freedoom)
 * or MUS (converted with mus2mid). Everything is handed to the C++ mixer in
 * DoomAudio.cpp, which the plug-in runs inside processBlock, ahead of the EQ.
 */
#include <stdint.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"
#include "m_misc.h"
#include "memio.h"
#include "mus2mid.h"
#include "w_wad.h"
#include "z_zone.h"

/* Implemented in DoomAudio.cpp */
void dg_audio_sfx_start (int channel, const unsigned char* pcm, int length, int rate, int vol, int sep);
void dg_audio_sfx_update (int channel, int vol, int sep);
void dg_audio_sfx_stop (int channel);
int  dg_audio_sfx_playing (int channel);
int  dg_audio_music_register (const void* midi, int length);
void dg_audio_music_play (int handle, int looping);
void dg_audio_music_stop (void);
void dg_audio_music_pause (int paused);
void dg_audio_music_volume (int volume);
int  dg_audio_music_playing (void);

/* Config variables i_sound.c binds (normally owned by the SDL backend). */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

static snddevice_t dg_devices[] = {
    SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32, SNDDEVICE_GENMIDI, SNDDEVICE_ADLIB
};

static boolean sfx_prefix = true;

/*----------------------------------------------------------------- sfx */

static boolean Snd_Init (boolean use_sfx_prefix)
{
    sfx_prefix = use_sfx_prefix;
    return true;
}

static void Snd_Shutdown (void) {}
static void Snd_Update (void) {}

static int Snd_GetSfxLumpNum (sfxinfo_t* sfx)
{
    char name[16];

    if (sfx->link != NULL)
        sfx = sfx->link;

    if (sfx_prefix)
        M_snprintf (name, sizeof (name), "ds%s", sfx->name);
    else
        M_StringCopy (name, sfx->name, sizeof (name));

    return W_CheckNumForName (name);
}

static void Snd_UpdateSoundParams (int channel, int vol, int sep)
{
    dg_audio_sfx_update (channel, vol, sep);
}

static int Snd_StartSound (sfxinfo_t* sfx, int channel, int vol, int sep)
{
    const unsigned char* data;
    int lump, length, rate, samples;

    lump = sfx->lumpnum;
    if (lump < 0)
        lump = Snd_GetSfxLumpNum (sfx);
    if (lump < 0)
        return -1;

    length = W_LumpLength (lump);
    data = (const unsigned char*) W_CacheLumpNum (lump, PU_STATIC);

    if (length < 8 || data[0] != 3 || data[1] != 0)
        return -1;

    rate = data[2] | (data[3] << 8);
    samples = (int) ((uint32_t) data[4] | ((uint32_t) data[5] << 8) | ((uint32_t) data[6] << 16) | ((uint32_t) data[7] << 24));

    if (samples > length - 8)
        samples = length - 8;

    data += 8;

    /* DMX pads both ends with 16 bytes. */
    if (samples > 48)
    {
        data += 16;
        samples -= 32;
    }

    if (rate <= 0 || samples <= 0)
        return -1;

    dg_audio_sfx_start (channel, data, samples, rate, vol, sep);
    return channel;
}

static void Snd_StopSound (int channel)
{
    dg_audio_sfx_stop (channel);
}

static boolean Snd_SoundIsPlaying (int channel)
{
    return dg_audio_sfx_playing (channel) ? true : false;
}

static void Snd_CacheSounds (sfxinfo_t* sounds, int num_sounds)
{
    (void) sounds;
    (void) num_sounds;
}

sound_module_t DG_sound_module = {
    dg_devices,
    (int) (sizeof (dg_devices) / sizeof (dg_devices[0])),
    Snd_Init,
    Snd_Shutdown,
    Snd_GetSfxLumpNum,
    Snd_Update,
    Snd_UpdateSoundParams,
    Snd_StartSound,
    Snd_StopSound,
    Snd_SoundIsPlaying,
    Snd_CacheSounds,
};

/*--------------------------------------------------------------- music */

static boolean Mus_Init (void) { return true; }
static void Mus_Shutdown (void) { dg_audio_music_stop(); }
static void Mus_SetVolume (int volume) { dg_audio_music_volume (volume); }
static void Mus_Pause (void) { dg_audio_music_pause (1); }
static void Mus_Resume (void) { dg_audio_music_pause (0); }

static void* Mus_Register (void* data, int len)
{
    int handle = 0;

    if (len > 4 && memcmp (data, "MUS\x1a", 4) == 0)
    {
        MEMFILE* in = mem_fopen_read (data, (size_t) len);
        MEMFILE* out = mem_fopen_write();

        if (! mus2mid (in, out))
        {
            void* buf;
            size_t buflen;
            mem_get_buf (out, &buf, &buflen);
            handle = dg_audio_music_register (buf, (int) buflen);
        }

        mem_fclose (in);
        mem_fclose (out);
    }
    else
    {
        handle = dg_audio_music_register (data, len);
    }

    return (void*) (intptr_t) handle;
}

static void Mus_Unregister (void* handle) { (void) handle; }

static void Mus_Play (void* handle, boolean looping)
{
    dg_audio_music_play ((int) (intptr_t) handle, looping ? 1 : 0);
}

static void Mus_Stop (void) { dg_audio_music_stop(); }
static boolean Mus_IsPlaying (void) { return dg_audio_music_playing() ? true : false; }
static void Mus_Poll (void) {}

music_module_t DG_music_module = {
    dg_devices,
    (int) (sizeof (dg_devices) / sizeof (dg_devices[0])),
    Mus_Init,
    Mus_Shutdown,
    Mus_SetVolume,
    Mus_Pause,
    Mus_Resume,
    Mus_Register,
    Mus_Unregister,
    Mus_Play,
    Mus_Stop,
    Mus_IsPlaying,
    Mus_Poll,
};
