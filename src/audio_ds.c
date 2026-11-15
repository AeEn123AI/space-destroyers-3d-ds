/* DS audio. Sound effects are short samples in RAM on hardware voices (maxmod), so they have no latency.
 * Music: the two lockstep streams (Stage: Intro->Main, Thumbnail: Options intro->Options select) are mono IMA-ADPCM
 * embedded in the ARM9 binary. Old and bootleg flashcart loaders may pass no argv, have no working DLDI and answer card
 * reads with their own data, so nothing here touches a filesystem. They are decoded + crossfaded in software with the
 * same sample count (so they can never drift apart) and fed to one maxmod stream. Thumbnail runs at half the rate.
 * A boot-time checksum catches a loader that truncated the image; the title screen then says music is unavailable. */
#include <nds.h>
#include <maxmod9.h>
#include <string.h>
#include "audio.h"
#include "music_gen.h"
#include "music_bin.h"
#include "soundbank.h"
#include "soundbank_bin.h"

#define RING 4096              /* 0.26 s at 16 kHz: rides out dropped frames and a records write */
enum { M_OFF, M_MUSIC, M_EGG };

static const mm_word sfx_ids[] = { SFX_POP, SFX_POPSTAGE, SFX_CRUNCH, SFX_HIT, SFX_LOSE };
static mm_sfxhand sfx_hand[SND_COUNT];
static int music_ok, mode, suspended_mode, drained;
static int vol_cur[GRP_COUNT] = { 256, 256, 256 }, vol_target[GRP_COUNT] = { 256, 256, 256 };

/* decoder state survives closing the stream, which is how sleep mode resumes where it was */
typedef struct { const u8 *data; u32 pos; s32 pred; int idx; } Stem;
static Stem st_a, st_b;
static u32 s_pos;              /* stage sample position */
static s32 t_prev;             /* thumbnail interpolation */

static const s16 ima_step[89] = {7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
    157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,
    2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,
    22385,24623,27086,29794,32767};
static const s8 ima_index[8] = {-1,-1,-1,-1,2,4,6,8};

ITCM_CODE static void ima_decode(Stem *s, int n, s16 *out) {
    s32 p = s->pred; int idx = s->idx; u32 pos = s->pos;
    const u8 *data = s->data;
    for (int i = 0; i < n; i++, pos++) {
        int code = (pos & 1) ? data[pos >> 1] >> 4 : data[pos >> 1] & 15;
        int step = ima_step[idx];
        int dq = step >> 3;
        if (code & 4) dq += step;
        if (code & 2) dq += step >> 1;
        if (code & 1) dq += step >> 2;
        p += (code & 8) ? -dq : dq;
        if (p > 32767) p = 32767; else if (p < -32768) p = -32768;
        idx += ima_index[code & 7];
        if (idx < 0) idx = 0; else if (idx > 88) idx = 88;
        out[i] = (s16)p;
    }
    s->pred = p; s->idx = idx; s->pos = pos;
}

static void enter_section(int main_part) {
    s_pos = main_part ? INTRO_SAMPLES : 0;
    st_a.data = music_bin;                st_a.pos = s_pos;     st_a.pred = main_part ? S1_PRED : S0_PRED; st_a.idx = IDX0;
    st_b.data = music_bin + STAGE_BYTES;  st_b.pos = s_pos / 2; st_b.pred = main_part ? T1_PRED : T0_PRED; st_b.idx = IDX0;
}

static mm_word on_stream(mm_word length, mm_addr dest, mm_stream_formats format) {
    (void)format;
    static s16 a[512], b[258];
    s16 *out = dest; int left = (int)length;
    for (int g = 0; g < 2; g++) {          /* volume ramp per request (about a frame): no zipper noise */
        int d = vol_target[g] - vol_cur[g];
        vol_cur[g] += d > 16 ? 16 : (d < -16 ? -16 : d);
    }
    const int va = vol_cur[GRP_STAGE], vb = vol_cur[GRP_THUMB];
    if (mode == M_EGG) {
        while (left > 0) {
            int n = EGG_SAMPLES - (int)st_a.pos;
            if (n <= 0) { memset(out, 0, left * 2); drained += left; break; }
            if (n > left) n = left;
            if (n > 512) n = 512;
            ima_decode(&st_a, n, a);
            for (int i = 0; i < n; i++) out[i] = (s16)((a[i] * va) >> 8);
            out += n; left -= n;
        }
        return length;
    }
    while (left > 0) {
        u32 end = s_pos < INTRO_SAMPLES ? INTRO_SAMPLES : TOTAL_SAMPLES;
        int n = (int)(end - s_pos);
        if (n > left) n = left;
        if (n > 512) n = 512;
        ima_decode(&st_a, n, a);
        int tn = (int)(((s_pos + n + 1) >> 1) - ((s_pos + 1) >> 1));   /* even stage positions in this block */
        ima_decode(&st_b, tn, b);
        s32 prev = t_prev; int tk = 0;
        for (int i = 0; i < n; i++) {
            s32 t;
            if (!((s_pos + i) & 1)) { s32 cur = b[tk++]; t = (prev + cur) >> 1; prev = cur; }   /* linear 2x */
            else t = prev;
            int s = (a[i] * va + t * vb) >> 8;
            out[i] = (s16)(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
        }
        t_prev = prev;
        s_pos += n; out += n; left -= n;
        /* Intro -> Main, and Main loops: the encoder restarts at each section, so the decoder has to as well */
        if (s_pos == end) enter_section(1);
    }
    return length;
}

static void stream_open(int m) {
    static mm_stream cfg;
    mode = m;
    cfg.sampling_rate = m == M_EGG ? EGG_RATE : MUSIC_RATE;
    cfg.buffer_length = RING;
    cfg.callback = on_stream;
    cfg.format = MM_STREAM_16BIT_MONO;
    cfg.timer = MM_TIMER0;
    cfg.manual = true;
    mmStreamOpen(&cfg);
}
static void stream_stop(void) {
    if (mode != M_OFF) { mmStreamClose(); mode = M_OFF; }
}

int audio_init(void) {
    mmInitDefaultMem((mm_addr)soundbank_bin);
    for (unsigned i = 0; i < sizeof sfx_ids / sizeof sfx_ids[0]; i++) mmLoadEffect(sfx_ids[i]);
    u32 sum = 0;
    const u32 *w = (const u32 *)music_bin;
    for (u32 i = 0; i < MUSIC_WORDS; i++) sum += w[i];
    music_ok = sum == MUSIC_SUM;
    return music_ok;
}

/* call once per frame */
void audio_update(void) {
    if (mode == M_OFF) return;
    mmStreamUpdate();
    if (mode == M_EGG && st_a.pos >= EGG_SAMPLES && drained >= RING) stream_stop();
}

/* sleep mode: the hardware stops, so stop cleanly and carry on from the decoder position on wake-up.
   What was queued but unplayed (under 0.26 s) is skipped; nobody can hear that across a closed lid. */
void audio_suspend(void) {
    mmEffectCancelAll();
    memset(sfx_hand, 0, sizeof sfx_hand);
    suspended_mode = mode;
    stream_stop();
}
void audio_resume(void) {
    if (suspended_mode == M_OFF) return;
    vol_cur[GRP_STAGE] = vol_cur[GRP_THUMB] = 0;   /* ramps back up: no click */
    stream_open(suspended_mode);
    suspended_mode = M_OFF;
}

void audio_play(int snd, int group) {
    (void)group;
    if (snd == SND_EASTER) {
        if (!music_ok) return;
        stream_stop();
        st_a.data = music_bin + STAGE_BYTES + THUMB_BYTES; st_a.pos = 0; st_a.pred = E_PRED; st_a.idx = IDX0;
        drained = 0;
        stream_open(M_EGG);
        return;
    }
    /* Scratch restarts a sound that is already playing */
    if (sfx_hand[snd]) mmEffectCancel(sfx_hand[snd]);
    sfx_hand[snd] = mmEffect(sfx_ids[snd]);
}

void audio_stop_all(void) {
    mmEffectCancelAll();
    memset(sfx_hand, 0, sizeof sfx_hand);
    stream_stop();
}

void audio_music_start(int with_intro) {
    if (!music_ok) return;
    stream_stop();
    enter_section(!with_intro);
    t_prev = 0;
    stream_open(M_MUSIC);
}

int audio_music_playing(void) { return mode == M_MUSIC || !music_ok; }   /* damaged data: don't ask again every frame */
int audio_sound_playing(int snd) { return snd == SND_EASTER && mode == M_EGG; }
void audio_set_group_volume(int g, float v) { vol_target[g] = v <= 0 ? 0 : (v >= 1 ? 256 : (int)(v * 256.0f)); }
