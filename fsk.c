/* usage: printf 'hello' | ./fsk | ffmpeg -y -f s16le -ar 11025 -i - /tmp/tmp.wav */
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
    const float samples_per_bit = sample_rate / baud;
    const float complex advance_mark = cexpf(I * 2.0f * (float)M_PI * f_mark / sample_rate);
    const float complex advance_space = cexpf(I * 2.0f * (float)M_PI * f_space / sample_rate);

    /* initial state */
    float complex carrier = 1.0f;
    float samples_since_bit_start = -10.0f * samples_per_bit;

    /* emit leading mark tone for ten bit periods to flush garbage out of decoder */
    for (; samples_since_bit_start < 0.0f; samples_since_bit_start++) {
        fwrite(&(int16_t) { lrintf(cimagf(carrier) * 32767.0f) }, sizeof(int16_t), 1, stdout);
        carrier *= advance_mark;
    }

    /* for each byte on stdin... */
    for (int byte; (byte = fgetc(stdin)) != EOF; )
        /* for each bit (including start and stop bits)... */
        for (size_t ibit = 0; ibit < 10; ibit++, samples_since_bit_start -= samples_per_bit) {
            const int bit = ibit < 1 ? 1 : ibit < 2 ? 0 : byte & (1 << (ibit - 2));

            /* for each sample, accounting for noninteger sample rate over baud... */
            for (; samples_since_bit_start < samples_per_bit; samples_since_bit_start++) {
                fwrite(&(int16_t) { lrintf(cimagf(carrier) * 32767.0f) }, sizeof(int16_t), 1, stdout);
                carrier *= bit ? advance_mark : advance_space;
            }

            /* assuming carrier is still near unity, renormalize to unity w/o div or sqrt */
            const float magsquared = cmagsquaredf(carrier);
            carrier *= (3.0f - magsquared) * 0.5f;
        }

    /* emit trailing mark tone for two bit periods to flush decoder */
    for (; samples_since_bit_start < 2.0f * samples_per_bit; samples_since_bit_start++) {
        fwrite(&(int16_t) { lrintf(cimagf(carrier) * 32767.0f) }, sizeof(int16_t), 1, stdout);
        carrier *= advance_mark;
    }
}
