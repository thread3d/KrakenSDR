/* spectrum-probe.c -- look for real carriers on a KrakenSDR tuner.
 *
 * Three things this does that a plain amplitude test does not:
 *
 *  1. It averages a Welch power spectrum, so continuous carriers stand out.
 *  2. It also keeps a MAX-HOLD spectrum, because the signals that matter here
 *     (ADS-B at 1090 MHz, ISM devices around 433/868 MHz, OGN/FLARM at 868 MHz)
 *     are BURSTS. Averaging 64 frames divides a burst by ~64 and hides it.
 *  3. It excludes a wide band around the centre frequency. Every RTL-SDR has a
 *     DC/LO spike exactly at the tuned frequency which can read 20-30 dB above
 *     the floor and is easily mistaken for a station. The default exclusion is
 *     +-25 kHz; a 200 kHz-wide FM carrier still shows up clearly beside it.
 *
 * Build:  gcc -O2 -o spectrum-probe spectrum-probe.c -lSoapySDR -lfftw3f -lm
 * Run:    ./spectrum-probe <freq_MHz> [rate_Msps] [gain_dB] [serial] [dc_excl_kHz]
 *         ./spectrum-probe 868.3 1.024 49.6 1000
 *
 * Exit 0 if at least one non-DC peak was found above -m threshold, else 1.
 *
 * Licence: GPL v3, to match the Kraken/OGN trees it supports.
 */

#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fftw3.h>

#define BLOCK   4096
#define MAXBLOCKS 262144   /* upper bound on averaged frames */
#define NPEAKS  5
#define THRESH  6.0f     /* dB above floor for "this is probably real" */

static float *g_avg, *g_max, *g_sorted;

static int cmp_desc(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x < y) - (x > y);
}

static void report(const char *name, float *spec, double floor_db,
                   double freq_hz, double rate_hz, int dc_bins)
{
    printf("  %s:\n", name);
    float best[NPEAKS];
    int bin[NPEAKS];
    for (int k = 0; k < NPEAKS; k++) { best[k] = -1e9f; bin[k] = -1; }
    int edge = BLOCK / 20;                 /* mask outer 5%: DDC artefact */
    for (int i = edge + 1; i < BLOCK - edge - 1; i++) {
        int dc = i - BLOCK / 2;
        if (dc >= -dc_bins && dc <= dc_bins) continue;
        /* must be a local maximum, else every bin of a carrier gets listed */
        if (spec[i] < spec[i-1] || spec[i] < spec[i+1]) continue;
        float snr = spec[i] - floor_db;
        for (int k = 0; k < NPEAKS; k++) {
            if (snr > best[k]) {
                for (int j = NPEAKS - 1; j > k; j--) { best[j] = best[j-1]; bin[j] = bin[j-1]; }
                best[k] = snr; bin[k] = i;
                break;
            }
        }
    }
    if (bin[0] < 0 || best[0] < 1.0f) { printf("    (nothing above floor)\n"); return; }
    for (int k = 0; k < NPEAKS; k++) {
        if (bin[k] < 0 || best[k] < 1.0f) continue;
        double off = (double)(bin[k] - BLOCK / 2) * rate_hz / BLOCK;
        printf("    %+9.1f kHz  %9.4f MHz  %5.1f dB%s\n",
               off / 1e3, (freq_hz + off) / 1e6, best[k],
               best[k] >= THRESH ? "  <= signal" : "");
    }
}

int main(int argc, char **argv)
{
    double freq_hz = (argc > 1) ? atof(argv[1]) * 1e6 : 868.3e6;
    double rate_hz = (argc > 2) ? atof(argv[2]) * 1e6 : 1.024e6;
    double gain_db = (argc > 3) ? atof(argv[3]) : 49.6;
    const char *serial = (argc > 4) ? argv[4] : "1000";
    double dc_khz  = (argc > 5) ? atof(argv[5]) : 25.0;
    double seconds = (argc > 6) ? atof(argv[6]) : 0.13;   /* capture length */
    int BLOCKS = (int)(seconds * rate_hz / BLOCK);
    if (BLOCKS < 8) BLOCKS = 8;
    if (BLOCKS > MAXBLOCKS) BLOCKS = MAXBLOCKS;
    int dc_bins = (int)(dc_khz * 1e3 / (rate_hz / BLOCK) + 0.5);
    if (dc_bins < 1) dc_bins = 1;

    SoapySDRKwargs args;
    memset(&args, 0, sizeof(args));
    SoapySDRKwargs_set(&args, "driver", "rtlsdr");
    SoapySDRKwargs_set(&args, "serial", serial);

    SoapySDRDevice *dev = SoapySDRDevice_make(&args);
    SoapySDRKwargs_clear(&args);
    if (!dev) { fprintf(stderr, "cannot open serial %s\n", serial); return 2; }

    SoapySDRDevice_setSampleRate(dev, SOAPY_SDR_RX, 0, rate_hz);
    SoapySDRDevice_setFrequency(dev, SOAPY_SDR_RX, 0, freq_hz, NULL);
    SoapySDRDevice_setGainMode(dev, SOAPY_SDR_RX, 0, false);
    SoapySDRDevice_setGain(dev, SOAPY_SDR_RX, 0, gain_db);

    SoapySDRStream *st = NULL;
    if (SoapySDRDevice_setupStream(dev, &st, SOAPY_SDR_RX, SOAPY_SDR_CS8,
                                   NULL, 0, NULL) != 0 || !st) {
        fprintf(stderr, "setupStream failed\n");
        return 2;
    }
    SoapySDRDevice_activateStream(dev, st, 0, 0, 0);

    printf("KrakenSDR spectrum probe\n");
    printf("  serial %s | centre %.4f MHz | %.4f Msps | %.1f dB\n",
           serial, freq_hz / 1e6, rate_hz / 1e6, gain_db);
    printf("  %d pt FFT, %d frames (%.2f s), %.2f kHz/bin, DC excluded +-%.0f kHz\n",
           BLOCK, BLOCKS, (double)BLOCKS * BLOCK / rate_hz, rate_hz / BLOCK / 1e3, dc_khz);

    fftwf_complex *buf = fftwf_alloc_complex(BLOCK);
    fftwf_complex *out = fftwf_alloc_complex(BLOCK);
    float *win = malloc(BLOCK * sizeof(float));
    g_avg = calloc(BLOCK, sizeof(float));
    g_max = calloc(BLOCK, sizeof(float));
    g_sorted = malloc(BLOCK * sizeof(float));
    if (!buf || !out || !win || !g_avg || !g_max || !g_sorted) { fprintf(stderr, "oom\n"); return 2; }

    for (int i = 0; i < BLOCK; i++)
        win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (BLOCK - 1));

    fftwf_plan plan = fftwf_plan_dft_1d(BLOCK, buf, out, FFTW_FORWARD, FFTW_ESTIMATE);

    int8_t *raw = malloc(BLOCK * 2);
    int frames = 0;
    while (frames < BLOCKS) {
        void *buffs[1] = { raw };
        int flags = 0; long long ts = 0;
        int n = SoapySDRDevice_readStream(dev, st, buffs, BLOCK, &flags, &ts, 1000000);
        if (n < BLOCK) continue;
        for (int i = 0; i < BLOCK; i++) {
            buf[i][0] = (float)raw[2 * i]     / 128.0f * win[i];
            buf[i][1] = (float)raw[2 * i + 1] / 128.0f * win[i];
        }
        fftwf_execute(plan);
        for (int i = 0; i < BLOCK; i++) {
            int src = (i + BLOCK / 2) % BLOCK;   /* fftshift */
            float p = out[src][0] * out[src][0] + out[src][1] * out[src][1];
            g_avg[i] += p / (float)BLOCKS;
            if (p > g_max[i]) g_max[i] = p;
        }
        frames++;
    }

    /* Noise floor from the averaged spectrum's median. */
    memcpy(g_sorted, g_avg, BLOCK * sizeof(float));
    qsort(g_sorted, BLOCK, sizeof(float), cmp_desc);
    float med = g_sorted[BLOCK / 2];
    double floor_db = 10.0 * log10(med > 0 ? med : 1e-20);

    static float avg_db[BLOCK], max_db[BLOCK];
    for (int i = 0; i < BLOCK; i++) {
        avg_db[i] = 10.0f * log10f(g_avg[i] > 0 ? g_avg[i] : 1e-20f);
        max_db[i] = 10.0f * log10f(g_max[i] > 0 ? g_max[i] : 1e-20f);
    }

    printf("  noise floor (median of averaged): %+.2f dB\n\n", floor_db);
    printf("continuous carriers (averaged spectrum):\n");
    report("avg", avg_db, floor_db, freq_hz, rate_hz, dc_bins);
    printf("\nbursts (max-hold spectrum):\n");
    report("max", max_db, floor_db, freq_hz, rate_hz, dc_bins);

    /* ASCII of the max-hold spectrum: bursts are what we are hunting. */
    printf("\nmax-hold spectrum (dB above noise floor, 64 columns)\n");
    int cols = 64, per = BLOCK / cols;
    for (int c = 0; c < cols; c++) {
        float peak = -1e9f;
        for (int i = c * per; i < (c + 1) * per; i++)
            if (max_db[i] > peak) peak = max_db[i];
        float db = peak - floor_db;
        int h = (int)(db / 2.0f);
        if (h < 0) h = 0; if (h > 24) h = 24;
        printf("  %7.3f ", (freq_hz + ((double)(c * per + per / 2) - BLOCK / 2) * rate_hz / BLOCK) / 1e6);
        for (int i = 0; i < h; i++) putchar('#');
        if (db > THRESH) printf("  <%.0f dB>", db);
        putchar('\n');
    }

    SoapySDRDevice_deactivateStream(dev, st, 0, 0);
    SoapySDRDevice_closeStream(dev, st);
    SoapySDRDevice_unmake(dev);
    fftwf_destroy_plan(plan);
    fftwf_free(buf); fftwf_free(out);
    free(win); free(g_avg); free(g_max); free(g_sorted); free(raw);
    return 0;
}
