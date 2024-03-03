/* consume raw s16 pcm samples at 11025 sps, containing bell 103 modulation, and emit bytes
 usage: ffmpeg -i /tmp/tmp.wav -f s16le -ar 11025 - | ./defsk */
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <complex.h>
#include <assert.h>
#include <stdint.h>

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

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* input arguments, all in cycles, samples, or symbols per second */
    const float sample_rate = 11025, f_mark = 1270, f_space = 1070, baud = 300;

    /* derived constants */
    const float samples_per_bit = sample_rate / baud;
    const float complex advance = cexpf(I * 2.0f * (float)M_PI * 0.5f * (f_mark + f_space) / sample_rate);

    /* compute filter coefficients for eight-pole butterworth biquad cascade */
    float num[4][3], den[4][3];
    butterworth_biquads(num, den, 8, sample_rate, fmaxf(baud, 1.5f * fabsf(f_mark - f_space)));
    float complex vprev[4][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } };

    /* the previous filter output, for consecutive-sample fm detection */
    float complex filtered_prev = 0.0f;

    /* current and previous decision value, for transition detection */
    int banged = 1, banged_previous = 1;

    /* more state variables */
    size_t ibit = 9;
    float samples_until_next_bit = samples_per_bit;

    /* stores the byte in progress */
    unsigned char byte = 0;

    /* loop over raw pcm samples on stdin */
    for (int16_t sample; fread(&sample, sizeof(int16_t), 1, stdin) > 0; ) {
        float complex filtered = sample;
        for (size_t is = 0; is < 4; is++)
            filtered = cfilter(filtered, vprev[is], num[is], den[is], advance);

        /* instantaneous frequency offset from centre of filter, in radians per second */
        const float arg = cargf(filtered * conjf(filtered_prev * advance)) * sample_rate;
        filtered_prev = filtered;

        /* a number between 0 and 1, with a bunch of noise and ripple */
        const float normalized = 0.5f + arg / (2.0f * (float)M_PI * (f_mark - f_space));

        /* either 0 or 1, with some hysteresis for debouncing */
        banged = banged ? (normalized < 0.25f ? 0 : 1) : (normalized < 0.75f ? 0 : 1);

        if (9 == ibit) {
            /* if we are not within a byte, and we see a down transition... */
            if (!banged && banged_previous && samples_until_next_bit <= 0.75f * samples_per_bit) {
                samples_until_next_bit = samples_per_bit * 1.5f;
                ibit = 0;
            }
        } else {
            /* whenever a transition is seen within a byte, update the time to next bit */
            if (banged != banged_previous)
                samples_until_next_bit = samples_per_bit * 0.5f;

            if (samples_until_next_bit <= 0.5f) {
                /* if this was the end-of-byte symbol... */
                if (8 == ibit) {
                    /* if the end-of-byte symbol was correct, emit complete byte */
                    if (1 == banged) putchar(byte);
                    else fprintf(stderr, "warning: %s: discarding possible %#x\n", __func__, byte);
                }
                /* otherwise set or clear this bit in the byte in progress */
                else byte = (byte & ~(1 << ibit)) | (banged ? (1 << ibit) : 0);

                ibit++;
                samples_until_next_bit += samples_per_bit;
            }
        }

        samples_until_next_bit--;
        banged_previous = banged;
    }
}
