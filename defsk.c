/* consume raw s16 pcm samples at 11025 sps, containing bell 203 modulation, and emit bytes
 usage: ffmpeg -i /tmp/tmp.wav -f s16le -ar 11025 - | ./defsk */
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <complex.h>
#include <assert.h>

static float cmagsquaredf(const float complex x) {
    return crealf(x) * crealf(x) + cimagf(x) * cimagf(x);
}

static void butterworth_biquads(float num[][3], float den[][3], size_t P, float fs, float fc) {
    /* number of poles must be even */
    assert(!(P % 2));

    /* prewarp corner frequency for bilinear transform */
    const float wc = 2.0f * tanf((float)M_PI * fc / fs);

    /* each stage implements a conjugate pair of analog poles */
    for (size_t is = 0; is < P / 2; is++) {
        /* analog butterworth pole. the two poles for this stage are this and its conjugate */
        const float complex apole = wc * cexpf(I * (float)M_PI * (2.0f * (is + 1) + P - 1.0f) / (2 * P));

        /* each analog pole results in one digital pole and one digital zero at -1.0 */
        const float complex dpole = (2.0f - apole) / (2.0f + apole);

        /* polynomial coefficients for pair of digital zeros at -1 */
        num[is][0] = 1.0f;
        num[is][1] = 2.0f;
        num[is][2] = 1.0f;

        /* polynomial coefficients for conjugate pair of digital poles */
        den[is][0] = dpole * conjf(dpole);
        den[is][1] = -2.0f * crealf(dpole);
        den[is][2] = 1.0f;

        /* normalize the set of coefficients for unit gain */
        const float den_sum = den[is][0] + den[is][1] + den[is][2];
        const float den_scale = 1.0f / den[is][0], num_scale = den_scale * den_sum / 4.0f;
        for (size_t ik = 0; ik < 3; ik++) num[is][ik] *= num_scale;
        for (size_t ik = 0; ik < 3; ik++) den[is][ik] *= den_scale;
    }
}

static float complex cfilter(const float complex x, float complex vprev[2], const float num[3],
                             const float den[3], const float complex advance) {
    /* operate on complex input and output with real filter coefficients and local carrier */
    const float complex v =          x - den[1] * vprev[0] - den[2] * vprev[1];
    const float complex y = num[0] * v + num[1] * vprev[0] + num[2] * vprev[1];

    vprev[1] = advance * vprev[0];
    vprev[0] = advance * v;

    return y;
}

static float butterworth_response(float f, float fc, float fs, unsigned n) {
    return 1.0f / (1.0f + powf(tanf((float)M_PI * f / fs) / tanf((float)M_PI * fc / fs), 2 * n));
}

int main(void) {
    /* input arguments, all in cycles, samples, or symbols per second */
    const float sample_rate = 11025, f_mark = 1270, f_space = 1070, baud = 300;

    /* derived constants */
    const size_t samples_per_bit = lrintf(sample_rate / baud);
    const float complex advance_mark = cexpf(I * 2.0f * (float)M_PI * f_mark / sample_rate);
    const float complex advance_space = cexpf(I * 2.0f * (float)M_PI * f_space / sample_rate);

    /* 3 dB cutoff frequency for low pass filters centered on each frequency */
    const float fc = 0.5f * baud;

    /* compute filter coefficients for four-pole butterworth biquad cascade */
    float num[2][3], den[2][3];
    butterworth_biquads(num, den, 4, sample_rate, fc);

    /* mag squared of response to space of the filter centered at mark, and vice versa */
    const float opp_response = butterworth_response(f_mark - f_space, fc, sample_rate, 4);

    /* scaling factor for combining filter outputs to obtain a decision value */
    const float normalize = 0.5f * (1.0f + opp_response) / (1.0f - opp_response);

    /* internal state for filters centered on each carrier */
    float complex vprev_mark[2][2] = { { 0, 0 }, { 0, 0 } };
    float complex vprev_space[2][2] = { { 0, 0 }, { 0, 0 } };

    /* output value, with hysteresis */
    int banged = 1;

    /* the previous sample, used to determine if there has been a transition */
    int banged_previous = 1;

    /* stores the byte in progress */
    unsigned char byte = 0;

    /* more state variables */
    size_t samples_since_last_transition = SIZE_MAX;
    size_t samples_since_last_bit = SIZE_MAX;
    size_t ibit = 9;

    /* loop over raw pcm samples on stdin */
    for (int16_t sample; fread(&sample, sizeof(int16_t), 1, stdin) > 0; ) {
        /* maintain filters around each of the two possible frequencies */
        const float mm = cmagsquaredf(cfilter(cfilter(sample, vprev_mark[0], num[0], den[0], advance_mark),
                                              vprev_mark[1], num[1], den[1], advance_mark));
        const float ss = cmagsquaredf(cfilter(cfilter(sample, vprev_space[0], num[0], den[0], advance_space),
                                              vprev_space[1], num[1], den[1], advance_space));

        /* a number between 0 and 1, with a bunch of noise and ripple */
        const float normalized = 0.5f + normalize * (mm - ss) / (mm + ss);

        /* either 0 or 1, with some hysteresis for debouncing */
        banged = banged ? (normalized < 0.4f ? 0 : 1) : (normalized < 0.6f ? 0 : 1);

        /* what follows is some very quick and dirty code from 2014 that takes oversampled,
         thresholded input and converts it to serial bytes, assuming 8N1 encoding, emitting
         framing errors to stderr. this was the first, most hackish thing that i came up
         with, and further attempts at cleverness at the time failed to come up with anything
         that worked better. nevertheless, significant improvements are likely possible */
        samples_since_last_transition++;
        samples_since_last_bit++;

        if (banged_previous != banged)
            samples_since_last_transition = 0;

        if (9 == ibit &&
            0 == banged &&
            samples_since_last_transition >= samples_per_bit / 2 &&
            samples_since_last_bit >= 3 * samples_per_bit / 2) {
            /* detected a down transition at the start of a byte */
            ibit = 0;
            samples_since_last_bit = 0;
        }
        else if (ibit < 9 && samples_since_last_bit == samples_per_bit) {
            /* if this was the end-of-byte symbol... */
            if (8 == ibit) {
                /* if the end-of-byte symbol was correct, emit complete byte */
                if (1 == banged) putchar(byte);

                /* otherwise print a warning to stderr */
                else fprintf(stderr, "warning: %s: discarding possible %#x\n", __func__, byte);
            } else {
                /* set or clear this bit in the byte in progress */
                byte = (byte & ~(1 << ibit)) | (banged ? (1 << ibit) : 0);

                samples_since_last_bit = 0;
            }

            ibit++;
        }

        banged_previous = banged;
    }
}
