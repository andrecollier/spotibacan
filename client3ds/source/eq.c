#include "eq.h"

#include <math.h>
#include <string.h>

#define EQ_RATE   44100.0f
#define EQ_BANDS  3

/* One RBJ-cookbook biquad, direct form I, with per-channel history. */
typedef struct {
	float b0, b1, b2, a1, a2;
	float x1[2], x2[2], y1[2], y2[2];
} biquad;

typedef enum { BQ_LOWSHELF, BQ_PEAK, BQ_HIGHSHELF } bq_kind;

typedef struct {
	bq_kind kind;
	float   hz, db, q;
} band_spec;

typedef struct {
	const char *key;
	float       gain_db;   /* applied before the limiter */
	band_spec   bands[EQ_BANDS];   /* db == 0 means unused */
} preset_spec;

/* Kept modest: the speakers are tiny and a big low shelf only buys
 * distortion. Boost is the loudness setting for the speakers on a bus. */
static const preset_spec PRESETS[EQ_COUNT] = {
	[EQ_OFF]    = { "off",    0.0f, {{0}} },
	[EQ_BASS]   = { "bass",   0.0f, {{ BQ_LOWSHELF, 180.0f, 6.0f, 0.7f }} },
	[EQ_TREBLE] = { "treble", 0.0f, {{ BQ_HIGHSHELF, 6000.0f, 5.0f, 0.7f }} },
	[EQ_VOCAL]  = { "vocal",  0.0f, {{ BQ_LOWSHELF, 120.0f, -3.0f, 0.7f },
	                                 { BQ_PEAK, 2500.0f, 4.0f, 1.0f }} },
	[EQ_BOOST]  = { "boost",  6.0f, {{ BQ_LOWSHELF, 150.0f, 5.0f, 0.7f },
	                                 { BQ_PEAK, 3000.0f, 2.0f, 0.9f },
	                                 { BQ_HIGHSHELF, 8000.0f, 3.0f, 0.7f }} },
};

static volatile int s_want = EQ_OFF;
static int          s_have = -1;
static biquad       s_bq[EQ_BANDS];
static int          s_nbands;
static float        s_gain = 1.0f;
static float        s_knee = 0.85f;

void eq_set(eq_preset p)
{
	if (p >= 0 && p < EQ_COUNT)
		s_want = p;
}

eq_preset eq_get(void) { return (eq_preset)s_want; }

const char *eq_key(eq_preset p)
{
	return (p >= 0 && p < EQ_COUNT) ? PRESETS[p].key : "off";
}

eq_preset eq_from_key(const char *key)
{
	for (int i = 0; i < EQ_COUNT; i++)
		if (key && strcmp(key, PRESETS[i].key) == 0)
			return (eq_preset)i;
	return EQ_OFF;
}

static void design(biquad *f, const band_spec *b)
{
	const float A = powf(10.0f, b->db / 40.0f);
	const float w0 = 2.0f * (float)M_PI * b->hz / EQ_RATE;
	const float cw = cosf(w0), sw = sinf(w0);
	const float alpha = sw / (2.0f * b->q);
	float b0, b1, b2, a0, a1, a2;
	if (b->kind == BQ_PEAK) {
		b0 = 1.0f + alpha * A;
		b1 = -2.0f * cw;
		b2 = 1.0f - alpha * A;
		a0 = 1.0f + alpha / A;
		a1 = -2.0f * cw;
		a2 = 1.0f - alpha / A;
	} else {
		const float sa = 2.0f * sqrtf(A) * alpha;
		if (b->kind == BQ_LOWSHELF) {
			b0 = A * ((A + 1) - (A - 1) * cw + sa);
			b1 = 2 * A * ((A - 1) - (A + 1) * cw);
			b2 = A * ((A + 1) - (A - 1) * cw - sa);
			a0 = (A + 1) + (A - 1) * cw + sa;
			a1 = -2 * ((A - 1) + (A + 1) * cw);
			a2 = (A + 1) + (A - 1) * cw - sa;
		} else {
			b0 = A * ((A + 1) + (A - 1) * cw + sa);
			b1 = -2 * A * ((A - 1) + (A + 1) * cw);
			b2 = A * ((A + 1) + (A - 1) * cw - sa);
			a0 = (A + 1) - (A - 1) * cw + sa;
			a1 = 2 * ((A - 1) - (A + 1) * cw);
			a2 = (A + 1) - (A - 1) * cw - sa;
		}
	}
	memset(f, 0, sizeof(*f));
	f->b0 = b0 / a0;
	f->b1 = b1 / a0;
	f->b2 = b2 / a0;
	f->a1 = a1 / a0;
	f->a2 = a2 / a0;
}

static void rebuild(int p)
{
	const preset_spec *ps = &PRESETS[p];
	s_nbands = 0;
	for (int i = 0; i < EQ_BANDS; i++)
		if (ps->bands[i].db != 0.0f)
			design(&s_bq[s_nbands++], &ps->bands[i]);
	s_gain = powf(10.0f, ps->gain_db / 20.0f);
	/* Only the boost preset leans on the limiter; the others merely guard
	 * against the clip their lift could cause. */
	s_knee = ps->gain_db > 0.0f ? 0.6f : 0.85f;
	s_have = p;
}

/* Gentle above ~0.6 of full scale, never past it: the boost preset leans
 * on this instead of clipping hard. A rational tanh stand-in, cheap on the
 * ARM11's VFP. */
static inline float soft_limit(float x, float t)
{
	float a = fabsf(x);
	if (a <= t)
		return x;
	float u = (a - t) / (1.0f - t);          /* 0.. */
	float c = u * (27.0f + u * u) / (27.0f + 9.0f * u * u);   /* ~tanh */
	if (c > 1.0f)
		c = 1.0f;
	float y = t + (1.0f - t) * c;
	return x < 0 ? -y : y;
}

void eq_process(int16_t *pcm, unsigned frames)
{
	const int want = s_want;
	if (want != s_have)
		rebuild(want);
	if (want == EQ_OFF)
		return;

	const float in_scale = s_gain / 32768.0f;
	for (unsigned i = 0; i < frames; i++) {
		for (int ch = 0; ch < 2; ch++) {
			float x = (float)pcm[i * 2 + ch] * in_scale;
			for (int b = 0; b < s_nbands; b++) {
				biquad *f = &s_bq[b];
				float y = f->b0 * x + f->b1 * f->x1[ch] + f->b2 * f->x2[ch]
				        - f->a1 * f->y1[ch] - f->a2 * f->y2[ch];
				f->x2[ch] = f->x1[ch];
				f->x1[ch] = x;
				f->y2[ch] = f->y1[ch];
				f->y1[ch] = y;
				x = y;
			}
			x = soft_limit(x, s_knee);
			int v = (int)(x * 32767.0f);
			pcm[i * 2 + ch] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
		}
	}
}
