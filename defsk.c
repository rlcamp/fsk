/* consume raw s16 pcm samples at 11025 sps, containing bell 203 modulation, and emit bytes
 usage: ffmpeg -i /tmp/tmp.wav -f s16le - | ./defsk
 */
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <complex.h>

static float cmagsquaredf(const float complex x) {
    return crealf(x) * crealf(x) + cimagf(x) * cimagf(x);
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

    /* compute alpha of an exponential filter centered at each of the two frequencies */
    const float tmp = cosf(2.0f * (float)M_PI * fc / sample_rate);
    const float alpha = tmp - 1.0f + sqrtf(tmp * tmp - 4.0f * tmp + 3.0f);
    const float one_minus_alpha = 1.0f - alpha;

    /* mag squared of response to space of the filter centered at mark, and vice versa */
    const float opp_response = cmagsquaredf(alpha / (1.0f - (1.0f - alpha) * cexpf(I * 2.0f * (float)M_PI * (f_mark - f_space) / sample_rate)));

    /* scaling factor for combining filter outputs to obtain a decision value */
    const float normalize = 0.5f * (1.0f + opp_response) / (1.0f - opp_response);

    /* response of the filters centered on each carrier */
    float complex mark = 0.0f, space = 0.0f;

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
        mark = sample + one_minus_alpha * (mark * advance_mark - sample);
        space = sample + one_minus_alpha * (space * advance_space - sample);

        /* power in the separate mark and space filters */
        const float mm = cmagsquaredf(mark), ss = cmagsquaredf(space);

        /* a number between 0 and 1, with a bunch of noise and ripple */
        const float normalized = 0.5f + normalize * (mm - ss) / (mm + ss);

        /* either 0 or 1, with some hysteresis for debouncing */
        banged = banged ? (normalized < 0.25f ? 0 : 1) : (normalized < 0.75f ? 0 : 1);

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
            samples_since_last_transition == samples_per_bit / 2 &&
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
                else fprintf(stderr, "warning: %s: discarding possible %#x\n",
                    __func__, byte);
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
