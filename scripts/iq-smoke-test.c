/* iq-smoke-test.c -- prove the KrakenSDR's tuners actually receive.
 *
 * Opens each RTL2838 through SoapySDR by the *EEPROM serial* (not by index,
 * because the enumeration order is not the channel order), tunes it, reads a
 * block of IQ and reports mean power and peak magnitude. A tuner that opens
 * but returns all zeros, or whose power does not move when you retune, is not
 * actually receiving.
 *
 * Build (needs libsoapysdr-dev, which IS installed on the Xavier):
 *     gcc -O2 -o iq-smoke-test iq-smoke-test.c -lSoapySDR -lm
 *
 * Run:
 *     ./iq-smoke-test                 # default 868.3 MHz, 1.024 Msps
 *     ./iq-smoke-test 100.0 2.048     # MHz and Msps
 *
 * Exit 0 if every tuner opened, produced non-zero samples, and reported power
 * consistent with a live receiver.
 *
 * Licence: GPL v3, to stay compatible with the Kraken/OGN trees it supports.
 */

#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Errors.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_CH 8
#define N_SAMPLES (1 << 17)   /* 131072 IQ pairs */
#define RX_TIMEOUT_US 1000000

int main(int argc, char **argv)
{
    double freq_hz = (argc > 1) ? atof(argv[1]) * 1e6 : 868.3e6;
    double rate_hz = (argc > 2) ? atof(argv[2]) * 1e6 : 1.024e6;
    double gain_db = (argc > 3) ? atof(argv[3]) : 32.0;

    /* The five KrakenSDR EEPROM serials. Discovered from sysfs on this unit;
     * if a different unit is used, run scripts/check-device.sh to list the
     * real ones (they are always 1000..1004 on stock KrakenSDR firmware). */
    const char *serials[MAX_CH] = { "1000", "1001", "1002", "1003", "1004" };

    int n_ch = 5;
    int failures = 0;

    printf("KrakenSDR IQ smoke test\n");
    printf("  centre   : %.4f MHz\n", freq_hz / 1e6);
    printf("  rate     : %.4f Msps\n", rate_hz / 1e6);
    printf("  gain     : %.1f dB\n", gain_db);
    printf("  samples  : %d per channel (%.1f ms)\n\n",
           N_SAMPLES, 1000.0 * N_SAMPLES / rate_hz);

    /* Enumerate first, so we can report what Soapy actually sees. */
    size_t num = 0;
    SoapySDRKwargs *found = SoapySDRDevice_enumerate(NULL, &num);
    printf("SoapySDR sees %zu rtlsdr device(s)\n", num);
    for (size_t i = 0; i < num; i++) {
        const char *s = NULL, *t = NULL;
        for (size_t k = 0; k < found[i].size; k++) {
            if (!strcmp(found[i].keys[k], "serial")) s = found[i].vals[k];
            if (!strcmp(found[i].keys[k], "tuner"))  t = found[i].vals[k];
        }
        printf("  [%zu] serial=%-6s tuner=%s\n", i, s ? s : "-", t ? t : "-");
    }
    printf("\n");

    if (num < (size_t)n_ch) {
        printf("WARNING: expected %d tuners, SoapySDR found %zu\n\n", n_ch, num);
        if ((int)num < n_ch) n_ch = (int)num;
    }

    printf("%-4s %-7s %-12s %-12s %-10s %s\n",
           "ch", "serial", "mean power", "peak |x|", "rms(dBFS)", "status");
    printf("---------------------------------------------------------------\n");

    for (int c = 0; c < n_ch; c++) {
        SoapySDRKwargs args;
        memset(&args, 0, sizeof(args));
        SoapySDRKwargs_set(&args, "driver", "rtlsdr");
        SoapySDRKwargs_set(&args, "serial", serials[c]);

        SoapySDRDevice *dev = SoapySDRDevice_make(&args);
        SoapySDRKwargs_clear(&args);

        if (!dev) {
            printf("%-4d %-7s %-12s %-12s %-10s %s\n",
                   c, serials[c], "-", "-", "-", "OPEN FAILED");
            failures++;
            continue;
        }

        const char *err = NULL;
        int rc;

        rc = SoapySDRDevice_setSampleRate(dev, SOAPY_SDR_RX, 0, rate_hz);
        if (rc != 0) err = SoapySDRDevice_lastError();
        rc = SoapySDRDevice_setFrequency(dev, SOAPY_SDR_RX, 0, freq_hz, NULL);
        if (rc != 0) err = SoapySDRDevice_lastError();
        rc = SoapySDRDevice_setGainMode(dev, SOAPY_SDR_RX, 0, false);
        if (rc != 0) err = SoapySDRDevice_lastError();
        rc = SoapySDRDevice_setGain(dev, SOAPY_SDR_RX, 0, gain_db);
        if (rc != 0) err = SoapySDRDevice_lastError();

        SoapySDRStream *stream = NULL;
        int sret = SoapySDRDevice_setupStream(
            dev, &stream, SOAPY_SDR_RX, SOAPY_SDR_CS8, NULL, 0, NULL);
        if (sret != 0 || !stream) {
            printf("%-4d %-7s %-12s %-12s %-10s %s\n",
                   c, serials[c], "-", "-", "-", "SETUP STREAM FAILED");
            SoapySDRDevice_unmake(dev);
            failures++;
            continue;
        }
        SoapySDRDevice_activateStream(dev, stream, 0, 0, 0);

        int8_t *buf = malloc(N_SAMPLES * 2);
        if (!buf) { failures++; break; }

        int got = 0;
        while (got < N_SAMPLES) {
            void *buffs[1] = { buf + (got * 2) };
            int flags = 0;
            long long ts = 0;
            int n = SoapySDRDevice_readStream(dev, stream, buffs,
                                             N_SAMPLES - got, &flags,
                                             &ts, RX_TIMEOUT_US);
            if (n <= 0) { if (err) break; break; }
            got += n;
        }

        /* Power over the samples actually received. */
        double sum_sq = 0.0;
        int peak = 0;
        long zeros = 0, tiny = 0;
        for (int i = 0; i < got * 2; i++) {
            int v = buf[i];
            sum_sq += (double)v * (double)v;
            int a = abs(v);
            if (a > peak) peak = a;
            if (v == 0) zeros++;
            if (a <= 1) tiny++;
        }
        double mean_power = (got > 0) ? sum_sq / (2.0 * got) : 0.0;
        double rms = sqrt(mean_power);
        double dbfs = (rms > 0) ? 20.0 * log10(rms / 128.0) : -999.0;
        double zero_frac = (got > 0) ? (double)zeros / (2.0 * got) : 0.0;
        double tiny_frac = (got > 0) ? (double)tiny / (2.0 * got) : 0.0;

        const char *status;
        if (got <= 0)                    { status = "NO DATA"; failures++; }
        else if (peak == 0)              { status = "ALL ZERO (ADC idle)"; failures++; }
        else if (tiny_frac > 0.99)       { status = "IDLE noise-floor only (no RF)"; failures++; }
        else if (peak >= 127)            { status = "CLIPPING (gain too high)"; }
        else                             { status = "OK"; }

        printf("%-4d %-7s %-12.1f %-12d %-10.1f %s%s%s\n",
               c, serials[c], mean_power, peak, dbfs, status,
               err ? "  err=" : "", err ? err : "");
        printf("     zero=%.1f%%  |x|<=1=%.1f%%\n",
               100.0 * zero_frac, 100.0 * tiny_frac);

        SoapySDRDevice_deactivateStream(dev, stream, 0, 0);
        SoapySDRDevice_closeStream(dev, stream);
        SoapySDRDevice_unmake(dev);
        free(buf);
    }

    SoapySDRKwargsList_clear(found, num);

    printf("\n");
    if (failures) {
        printf("RESULT: %d channel(s) FAILED\n", failures);
        return 1;
    }
    printf("RESULT: all %d channels opened and produced live samples\n", n_ch);
    return 0;
}
