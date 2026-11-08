# 05 — ogn-rf startup stall: FFTW float planning

A focused technical note, because this one is subtle, cost a long time to find, and will bite
again on any ARM host.

## Symptom

`ogn-rf` built from branch `krakensdr` starts, then sits at **~100 % of one core, single-threaded,
in user space, producing no output at all** — not even from statements at the very top of `main()`.
It does not exit, does not open the USB device (no `/dev/bus/usb` file descriptor), and never creates
its acquisition thread (still one entry in `/proc/<pid>/task`).

Observed for **over three minutes** with `utime` tracking wall time one-for-one: it is burning CPU,
not blocked. `state=R`, `wchan=0`.

Because nothing is printed, it looks like a deadlock or an infinite loop in a static initialiser.

## It is not a deadlock — it is FFTW planning

`ogn-rf` creates five file-scope objects with constructors (in `ogn-rf.cc`, just before `main`):

```cpp
RF_Acq             RF;
Inp_Filter<float>  Filter(&RF);      // ctor -> Config_Defaults(); Preset();
Inp_FFT<float>     FFT(&RF, &Filter);
GSM_FFT<float>     GSM(&RF);
HTTP_Server<float> HTTP(&RF, &GSM);
```

`Inp_Filter`'s constructor calls `Preset()` → `ToneFilt.Preset()` with `ToneFilt.FFTsize = 32768`
set moments earlier by `Config_Defaults()`. `Inp_FFT`'s constructor calls
`Preset(RF->SampleRate)` → a 4096-point plan. So **three float FFTs are planned during static
initialisation, before `main()` is entered.**

And `fft.h` requests the most expensive possible plan:

```c
fft.h:55   Plan = fftw_plan_dft_1d (Size, ..., Sign, FFTW_MEASURE);   // double
fft.h:97   Plan = fftwf_plan_dft_1d(Size, ..., Sign, FFTW_MEASURE);   // float  <-- ogn-rf uses this
```

`FFTW_MEASURE` benchmarks candidate algorithms at runtime. That is normally a good trade, but on this
host the **single-precision** planner is pathologically slow.

## Measured

`scripts/fftw-plan-timing.c` times plan creation in isolation, same process, same library:

| transform | points | `FFTW_MEASURE` plan time |
|---|---|---|
| float (`fftwf`) | 4 096 | **37.144 s** |
| float (`fftwf`) | 32 768 | **60.858 s** |
| double (`fftw`) | 4 096 | **0.022 s** |
| double (`fftw`) | 32 768 | **0.038 s** |

Double precision plans in tens of milliseconds. Float precision takes a minute. That is a
**~1 700×** discrepancy between two transforms of identical size in the same library, which is not
normal FFTW behaviour — it points at something wrong with the `libfftw3f` build on this system
(Ubuntu Focal `libfftw3f3` on `aarch64`), most likely no usable NEON codelet path so the planner
walks a large space of slow scalar alternatives before settling.

`ogn-rf` plans three float transforms at startup, so it spends **well over 100 seconds** in
`fftwf_plan_dft_1d` before `main()` runs. It is not hung; it is planning.

## Reproduce it

```bash
gcc -O2 -o fftwt scripts/fftw-plan-timing.c -lfftw3 -lfftw3f
./fftwt
```

If the float lines take seconds-to-minutes and the double lines are instant, this is your problem.

## Fixes, in order of preference

1. **Cache wisdom.** Keep `FFTW_MEASURE` — the plans genuinely are faster at runtime — but pay for
   it once and persist the result:
   ```c
   fftwf_import_wisdom_from_filename("ogn-rf.fftwf.wisdom");   // before planning
   ... plan with FFTW_MEASURE or FFTW_WISDOM_ONLY ...
   fftwf_export_wisdom_to_filename("ogn-rf.fftwf.wisdom");     // after
   ```
   First run is slow, every later run is instant, and the plans stay optimal. This is the right
   answer for a fixed-purpose appliance like an OGN receiver.
2. **Use `FFTW_ESTIMATE`.** Immediate startup, deterministic, no disk state — but measurably slower
   per transform. For a 1 MS/s receiver doing 4096-point FFTs every slot this is probably fine, and
   it is a two-line change.
3. **Plan lazily, after `main()` and after configuration.** Even with the stall, it would at least
   happen once the process is logging, so the symptom becomes visible instead of looking like a hang.
   Worth doing regardless of 1 or 2.
4. **Use double precision.** Instant plans (0.038 s), but doubles the memory traffic and changes
   numerics — only worth it if 1 and 2 both prove unacceptable.

The `krakensdr` branch should take **1 + 3**: wisdom caching plus moving planning out of static
initialisation.

## Related trap for the same session

`ogn-rf`'s `main()` runs a command loop reading **stdin**:

```cpp
while(!RF.StopReq)
{ if(fgets(Cmd, 128, stdin)==0) break;
  UserCommand(Cmd); }
```

If stdin is at EOF — which is exactly what happens when you run it from a script, from `nohup`, or
under a service manager that gives it `/dev/null` — it breaks out, sleeps 4 s, stops the acquisition
thread and exits **immediately**, having done nothing. It needs a stdin that stays open (a pty, or
`sleep infinity | ogn-rf ...`). When diagnosing, this alone will convince you the program is broken
in a completely different way.

Also note `printf` output is block-buffered when stdout is a pipe, so `timeout`-killing the process
loses everything it printed. Use `stdbuf -oL -eL` or redirect to a file.
