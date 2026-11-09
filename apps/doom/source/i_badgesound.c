// SPDX-License-Identifier: GPL-2.0-or-later
//
// DOOM's sound effects on the badge: a sound module for fbDOOM's i_sound.c
// that mixes the WAD's effects (DMX lumps: 8-bit unsigned, mostly 11025 Hz)
// into 22050 Hz stereo for bsound (sdapps/sound/bsound.h), which plays it on
// the speaker or an AirPlay speaker. Each effect is copied out of the WAD the
// first time it plays and kept, so the mixer never reads the zone, which DOOM
// may purge.
//

#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "deh_str.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#include "bsound.h"

#define RATE		22050
#define CHANNELS	16

typedef struct
{
    byte *data;                 // unsigned 8-bit samples
    unsigned int length;        // samples
    unsigned int step;          // 16.16: the effect's samples per output frame
} badge_sfx_t;

typedef struct
{
    badge_sfx_t *sfx;           // NULL: the channel is silent
    unsigned int pos;           // 16.16
    int left, right;            // 0..127 * 2
} badge_channel_t;

static badge_channel_t channels[CHANNELS];
static boolean use_prefix;
static boolean started;

static void Mix(void *user, int16_t *out, unsigned int frames)
{
    unsigned int f;
    int c;

    (void) user;
    for (f = 0; f < frames; ++f)
    {
        int l = 0, r = 0;

        for (c = 0; c < CHANNELS; ++c)
        {
            badge_channel_t *ch = &channels[c];
            int s;

            if (ch->sfx == NULL)
            {
                continue;
            }
            s = ((int) ch->sfx->data[ch->pos >> 16] - 128) << 8;
            l += s * ch->left >> 9;     // half: room for a few at once
            r += s * ch->right >> 9;
            ch->pos += ch->sfx->step;
            if ((ch->pos >> 16) >= ch->sfx->length)
            {
                ch->sfx = NULL;
            }
        }
        out[2 * f] = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
        out[2 * f + 1] = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
    }
}

static void GetSfxLumpName(sfxinfo_t *sfx, char *buf, size_t buf_len)
{
    if (sfx->link != NULL)
    {
        sfx = sfx->link;
    }
    if (use_prefix)
    {
        M_snprintf(buf, buf_len, "ds%s", DEH_String(sfx->name));
    }
    else
    {
        M_StringCopy(buf, DEH_String(sfx->name), buf_len);
    }
}

static int I_Badge_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];

    GetSfxLumpName(sfx, namebuf, sizeof(namebuf));
    return W_GetNumForName(namebuf);
}

// The effect as the mixer reads it, copied out of the WAD: NULL if it is no DMX sound.
static badge_sfx_t *Load(sfxinfo_t *sfxinfo)
{
    badge_sfx_t *s;
    byte *data;
    unsigned int lumplen, rate, length;

    if (sfxinfo->link != NULL)
    {
        sfxinfo = sfxinfo->link;
    }
    if (sfxinfo->driver_data != NULL)
    {
        return sfxinfo->driver_data;
    }
    if (sfxinfo->lumpnum < 0)
    {
        sfxinfo->lumpnum = I_Badge_GetSfxLumpNum(sfxinfo);
    }
    data = W_CacheLumpNum(sfxinfo->lumpnum, PU_STATIC);
    lumplen = W_LumpLength(sfxinfo->lumpnum);

    // a header (format 3, the rate, the length), 16 bytes of padding, the
    // samples, 16 more bytes of padding
    rate = data[2] | data[3] << 8;
    length = data[4] | data[5] << 8 | data[6] << 16 | (unsigned int) data[7] << 24;
    if (lumplen < 8 || data[0] != 3 || data[1] != 0 || rate == 0
     || length > lumplen - 8 || length <= 48)
    {
        W_ReleaseLumpNum(sfxinfo->lumpnum);
        return NULL;
    }
    length -= 32;
    s = malloc(sizeof(*s) + length);
    if (s == NULL)
    {
        W_ReleaseLumpNum(sfxinfo->lumpnum);
        return NULL;
    }
    s->data = (byte *) (s + 1);
    memcpy(s->data, data + 8 + 16, length);
    s->length = length;
    s->step = (unsigned int) (((unsigned long long) rate << 16) / RATE);
    W_ReleaseLumpNum(sfxinfo->lumpnum);
    sfxinfo->driver_data = s;
    return s;
}

static void Volumes(badge_channel_t *ch, int vol, int sep)
{
    // sep 0 is the left, 254 the right; vol 0..127
    ch->left = ((254 - sep) * vol) / 127;
    ch->right = (sep * vol) / 127;
}

static void I_Badge_UpdateSoundParams(int handle, int vol, int sep)
{
    if (handle < 0 || handle >= CHANNELS)
    {
        return;
    }
    bsound_lock();
    Volumes(&channels[handle], vol, sep);
    bsound_unlock();
}

static int I_Badge_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    badge_sfx_t *s;

    if (channel < 0 || channel >= CHANNELS)
    {
        return -1;
    }
    s = Load(sfxinfo);
    bsound_lock();
    channels[channel].sfx = s;
    channels[channel].pos = 0;
    Volumes(&channels[channel], vol, sep);
    bsound_unlock();
    return s != NULL ? channel : -1;
}

static void I_Badge_StopSound(int handle)
{
    if (handle < 0 || handle >= CHANNELS)
    {
        return;
    }
    bsound_lock();
    channels[handle].sfx = NULL;
    bsound_unlock();
}

static boolean I_Badge_SoundIsPlaying(int handle)
{
    if (handle < 0 || handle >= CHANNELS)
    {
        return false;
    }
    return channels[handle].sfx != NULL;
}

static void I_Badge_UpdateSound(void)
{
}

static void I_Badge_ShutdownSound(void)
{
    if (started)
    {
        bsound_stop();
        started = false;
    }
}

static boolean I_Badge_InitSound(boolean _use_sfx_prefix)
{
    use_prefix = _use_sfx_prefix;
    memset(channels, 0, sizeof(channels));
    started = bsound_start(RATE, 2, Mix, NULL) == 0;
    return started;
}

static snddevice_t sound_badge_devices[] =
{
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

sound_module_t sound_badge_module =
{
    sound_badge_devices,
    arrlen(sound_badge_devices),
    I_Badge_InitSound,
    I_Badge_ShutdownSound,
    I_Badge_GetSfxLumpNum,
    I_Badge_UpdateSound,
    I_Badge_UpdateSoundParams,
    I_Badge_StartSound,
    I_Badge_StopSound,
    I_Badge_SoundIsPlaying,
    NULL,
};
