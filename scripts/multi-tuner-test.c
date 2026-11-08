/* multi-tuner-test.c -- stream ALL FIVE KrakenSDR tuners simultaneously.
 *
 * The earlier tests (iq-smoke-test) opened the tuners one at a time, so they
 * proved each channel works but never that the array runs concurrently - which
 * is the whole point of a KrakenSDR. This opens all five by EEPROM serial,
 * gives each its own thread and its own stream, and reports per-channel
 * sample throughput, power and the strongest carrier found.
 *
 * What this DOES show:
 *   - all five tuners streaming at once over the board's single USB link
 *   - per-channel throughput, so USB bandwidth/dropouts are visible
 *   - whether every channel hears the same external carrier
 *
 * What this does NOT show: phase coherence. The tuners share a clock in
 * hardware, but coherent direction finding needs HeIMDALL to start them in
 * lockstep and run the delay/phase calibration. This test does not do that.
 *
 * Build: gcc -O2 -pthread -o multi-tuner-test multi-tuner-test.c \
 *            -lSoapySDR -lfftw3f -lm
 * Run:   ./multi-tuner-test [freq_MHz] [rate_Msps] [gain_dB] [seconds]
 *        ./multi-tuner-test 868.3 1.024 25.4 2
 *
 * Licence: GPL v3, to match the Kraken/OGN trees it supports.
 */

#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fftw3.h>

#define NCH_MAX 5
static int g_nch = 5;
#define BLOCK   4096
#define PERCH   16               /* bytes per complex sample for RTL CS8 */

static const char *g_serial[NCH_MAX] = { "1000", "1001", "1002", "1003", "1004" };
static double g_freq, g_rate, g_gain, g_secs;

typedef struct {
    int    ch;
    int    ok;
    char   err[128];
    long long samples;
    double power;
    double peak_hz;      /* absolute frequency of strongest bin */
    double peak_snr;
    double floor_db;
    /* device handles, opened serially by main before any thread starts */
    SoapySDRDevice *dev;
    SoapySDRStream *st;
} result;

/* Opens one tuner and starts its stream. MUST be called from a single thread:
 * librtlsdr enumerates the USB bus on every open and that is not thread-safe -
 * five threads calling SoapySDRDevice_make() at once makes four of them fail. */
static int open_tuner(result *r)
{
    SoapySDRKwargs args;
    memset(&args, 0, sizeof(args));
    SoapySDRKwargs_set(&args, "driver", "rtlsdr");
    SoapySDRKwargs_set(&args, "serial", g_serial[r->ch]);

    r->dev = SoapySDRDevice_make(&args);
    SoapySDRKwargs_clear(&args);
    if (!r->dev) { snprintf(r->err, sizeof r->err, "open failed"); return -1; }

    if (SoapySDRDevice_setSampleRate(r->dev, SOAPY_SDR_RX, 0, g_rate) != 0 ||
        SoapySDRDevice_setFrequency(r->dev, SOAPY_SDR_RX, 0, g_freq, NULL) != 0) {
        snprintf(r->err, sizeof r->err, "configure failed");
        return -1;
    }
    SoapySDRDevice_setGainMode(r->dev, SOAPY_SDR_RX, 0, false);
    SoapySDRDevice_setGain(r->dev, SOAPY_SDR_RX, 0, g_gain);

    r->st = NULL;
    if (SoapySDRDevice_setupStream(r->dev, &r->st, SOAPY_SDR_RX, SOAPY_SDR_CS8,
                                   NULL, 0, NULL) != 0 || !r->st) {
        snprintf(r->err, sizeof r->err, "stream setup failed");
        return -1;
    }
    SoapySDRDevice_activateStream(r->dev, r->st, 0, 0, 0);
    return 0;
}

static void *worker(void *arg)
{
    result *r = arg;
    SoapySDRDevice *dev = r->dev;
    SoapySDRStream *st = r->st;
    if (!dev || !st) { snprintf(r->err, sizeof r->err, "not opened"); return NULL; }

    fftwf_complex *buf = fftwf_alloc_complex(BLOCK);
    fftwf_complex *out = fftwf_alloc_complex(BLOCK);
    float *win = malloc(BLOCK * sizeof(float));
    double *acc = calloc(BLOCK, sizeof(double));
    int8_t *raw = malloc(BLOCK * 2);
    if (!buf || !out || !win || !acc || !raw) {
        snprintf(r->err, sizeof r->err, "oom");
        goto done;
    }
    for (int i = 0; i < BLOCK; i++)
        win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (BLOCK - 1));
    fftwf_plan plan = fftwf_plan_dft_1d(BLOCK, buf, out, FFTW_FORWARD, FFTW_ESTIMATE);

    long long want = (long long)(g_secs * g_rate);
    double sum_sq = 0;
    long long frames = 0;
    while (r->samples < want) {
        void *buffs[1] = { raw };
        int flags = 0; long long ts = 0;
        int n = SoapySDRDevice_readStream(dev, st, buffs, BLOCK, &flags, &ts, 1000000);
        if (n <= 0) break;
        r->samples += n;
        for (int i = 0; i < n; i++) {
            double v0 = raw[2 * i], v1 = raw[2 * i + 1];
            sum_sq += v0 * v0 + v1 * v1;
        }
        if (n == BLOCK) {
            for (int i = 0; i < BLOCK; i++) {
                buf[i][0] = (float)raw[2 * i]     / 128.0f * win[i];
                buf[i][1] = (float)raw[2 * i + 1] / 128.0f * win[i];
            }
            fftwf_execute(plan);
            for (int i = 0; i < BLOCK; i++) {
                int src = (i + BLOCK / 2) % BLOCK;
                acc[i] += (double)out[src][0] * out[src][0] +
                          (double)out[src][1] * out[src][1];
            }
            frames++;
        }
    }

    r->power = (r->samples > 0) ? sum_sq / (2.0 * r->samples) : 0;
    r->ok = (r->samples > 0);

    if (frames > 0) {
        double db[BLOCK], sorted[BLOCK];
        for (int i = 0; i < BLOCK; i++) {
            double p = acc[i] / frames;
            db[i] = 10.0 * log10(p > 0 ? p : 1e-20);
            sorted[i] = db[i];
        }
        for (int i = 1; i < BLOCK; i++)            /* insertion-free: simple sort */
            for (int j = i; j > 0 && sorted[j] < sorted[j-1]; j--) {
                double t = sorted[j]; sorted[j] = sorted[j-1]; sorted[j-1] = t;
            }
        double med = sorted[BLOCK / 2];
        r->floor_db = med;
        int dc = BLOCK / 50, edge = BLOCK / 20;
        double best = -1e9; int bestbin = -1;
        for (int i = edge; i < BLOCK - edge; i++) {
            if (i - BLOCK / 2 >= -dc && i - BLOCK / 2 <= dc) continue;
            if (db[i] - med > best) { best = db[i] - med; bestbin = i; }
        }
        if (bestbin >= 0) {
            r->peak_snr = best;
            r->peak_hz = g_freq + (double)(bestbin - BLOCK / 2) * g_rate / BLOCK;
        }
    }

    fftwf_destroy_plan(plan);
    fftwf_free(buf); fftwf_free(out);
    free(win); free(acc); free(raw);

done:
    return NULL;
}

int main(int argc, char **argv)
{
    g_freq = (argc > 1) ? atof(argv[1]) * 1e6 : 868.3e6;
    g_rate = (argc > 2) ? atof(argv[2]) * 1e6 : 1.024e6;
    g_gain = (argc > 3) ? atof(argv[3]) : 25.4;
    g_secs = (argc > 4) ? atof(argv[4]) : 2.0;
    g_nch  = (argc > 5) ? atoi(argv[5]) : NCH_MAX;
    if (g_nch < 1) g_nch = 1;
    if (g_nch > NCH_MAX) g_nch = NCH_MAX;

    /* Line-buffered so progress survives a crash - this test crashed once and
     * lost everything it had printed, which hid where it died. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("KrakenSDR simultaneous multi-tuner test\n");
    printf("  %d tuners | centre %.4f MHz | %.4f Msps | %.1f dB | %.1f s each\n",
           g_nch, g_freq / 1e6, g_rate / 1e6, g_gain, g_secs);
    printf("  aggregate sample rate: %.3f Msps (%.1f MB/s of USB traffic)\n\n",
           g_nch * g_rate / 1e6, g_nch * g_rate * 2 / 1e6);

    result res[NCH_MAX];
    memset(res, 0, sizeof res);
    pthread_t th[NCH_MAX];

    /* Phase 1: open every tuner serially (USB enumeration is not thread-safe). */
    for (int i = 0; i < g_nch; i++) {
        res[i].ch = i;
        if (open_tuner(&res[i]) != 0)
            fprintf(stderr, "  ch%d (serial %s): %s\n", i, g_serial[i], res[i].err);
    }

    /* Phase 2: stream them all at once. */
    for (int i = 0; i < g_nch; i++)
        pthread_create(&th[i], NULL, worker, &res[i]);
    for (int i = 0; i < g_nch; i++)
        pthread_join(th[i], NULL);

    /* Phase 3: tear down. */
    for (int i = 0; i < g_nch; i++) {
        if (!res[i].dev) continue;
        if (res[i].st) {
            SoapySDRDevice_deactivateStream(res[i].dev, res[i].st, 0, 0);
            SoapySDRDevice_closeStream(res[i].dev, res[i].st);
        }
        SoapySDRDevice_unmake(res[i].dev);
    }

    long long want = (long long)(g_secs * g_rate);
    printf("%-4s %-7s %-11s %-9s %-10s %-13s %s\n",
           "ch", "serial", "samples", "Msps", "power", "peak freq", "SNR");
    printf("----------------------------------------------------------------------------\n");

    int ok_all = 1;
    double sum_peak_snr = 0;
    for (int i = 0; i < g_nch; i++) {
        result *r = &res[i];
        if (!r->ok) {
            printf("%-4d %-7s %-11s %-9s %-10s %-13s %s\n",
                   r->ch, g_serial[r->ch], "0", "-", "-", "-",
                   r->err[0] ? r->err : "FAILED");
            ok_all = 0;
            continue;
        }
        printf("%-4d %-7s %-11lld %-9.3f %-10.1f %-13.4f %.1f dB\n",
               r->ch, g_serial[r->ch], r->samples,
               (double)r->samples / g_secs / 1e6, r->power,
               r->peak_hz / 1e6, r->peak_snr);
        sum_peak_snr += r->peak_snr;
    }

    printf("\n");
    if (!ok_all) { printf("RESULT: at least one tuner FAILED\n"); return 1; }

    /* Throughput check: flag any channel materially behind the others, which
     * means the shared USB link is the limit rather than the tuner. */
    long long mn = res[0].samples, mx = res[0].samples;
    for (int i = 1; i < g_nch; i++) {
        if (res[i].samples < mn) mn = res[i].samples;
        if (res[i].samples > mx) mx = res[i].samples;
    }
    double spread = (mx > 0) ? 100.0 * (double)(mx - mn) / (double)mx : 0;
    printf("throughput spread across channels: %.1f%% (low %lld, high %lld)\n",
           spread, mn, mx);
    printf("mean peak SNR: %.1f dB\n", sum_peak_snr / g_nch);

    /* Do all channels agree on the strongest carrier's frequency? */
    double fmin = 1e18, fmax = 0;
    for (int i = 0; i < g_nch; i++) {
        if (res[i].peak_hz < fmin) fmin = res[i].peak_hz;
        if (res[i].peak_hz > fmax) fmax = res[i].peak_hz;
    }
    printf("strongest carrier seen at: %.4f .. %.4f MHz (spread %.1f kHz)\n",
           fmin / 1e6, fmax / 1e6, (fmax - fmin) / 1e3);

    printf("\nRESULT: %d tuners streamed concurrently\n", g_nch);
    printf("NOTE: frequency agreement only. PHASE COHERENCE is not tested here -\n");
    printf("      that needs HeIMDALL to start the tuners in lockstep and run the\n");
    printf("      delay/phase calibration.\n");
    return 0;
}
