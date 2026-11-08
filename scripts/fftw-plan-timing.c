#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <fftw3.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}

int main(void)
{
    size_t sizes[] = { 4096, 32768 };
    int n = sizeof(sizes) / sizeof(sizes[0]);

    for (int i = 0; i < n; i++) {
        size_t sz = sizes[i];

        fftwf_complex *fb = fftwf_malloc(sz * sizeof(fftwf_complex));
        double t0 = now();
        fftwf_plan fp = fftwf_plan_dft_1d(sz, fb, fb, FFTW_FORWARD, FFTW_MEASURE);
        double t1 = now();
        printf("float  %6zu pt FFTW_MEASURE: %7.3f s  plan=%p\n", sz, t1 - t0, (void *)fp);
        fflush(stdout);

        fftw_complex *db = fftw_malloc(sz * sizeof(fftw_complex));
        t0 = now();
        fftw_plan dp = fftw_plan_dft_1d(sz, db, db, FFTW_FORWARD, FFTW_MEASURE);
        t1 = now();
        printf("double %6zu pt FFTW_MEASURE: %7.3f s  plan=%p\n", sz, t1 - t0, (void *)dp);
        fflush(stdout);

        fftwf_destroy_plan(fp);
        fftw_destroy_plan(dp);
        fftwf_free(fb);
        fftw_free(db);
    }
    return 0;
}
