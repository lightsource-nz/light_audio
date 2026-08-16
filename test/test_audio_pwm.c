// checks for the PWM audio driver -- the layer between light_audio's core and the PWM hardware.
//
// WHY THIS EXISTS: audio_pwm.c sat at 0% coverage, and it was tempting to write that off as
// "hardware code, untestable on a host". It is not. Every hardware access it makes goes through
// the light_platform_pwm_* port API, which is exactly the seam needed to test it: replace that
// layer and the driver runs anywhere.
//
// WHAT IT ACTUALLY GUARDS: the behaviour here is all about not making unpleasant noises, and
// none of it is visible by reading the calls in isolation --
//   - the PWM is parked at mid-scale duty on open, so the first sample does not step the DC
//     level and click through a piezo,
//   - a tone of 0 Hz drives the pin LOW rather than just stopping, because a piezo across a
//     floating pin hisses,
//   - teardown hands the pin back low and then closes it, because closing alone leaves the pin
//     wherever the last duty put it, and leaks the PWM block besides.
// Each of those is a one-line call whose absence is silent in code review and audible on a bench.
//
// HOW THE FAKE IS INSTALLED: -Wl,--wrap, not a replacement source file. light_core_arch_host_os
// contributes light_platform_pwm.c to every target as an INTERFACE source, so defining these
// symbols again here would be a duplicate definition rather than an override. --wrap redirects
// each call to __wrap_<name> at link time and leaves the real implementation untouched. See
// test/CMakeLists.txt for the flags, which must list every symbol the driver calls.
//
// RUN AS: ctest, or this binary directly. With no argument it runs everything; with a case name
// it runs just that one, which is how CTest registers them individually.
#include <light_audio.h>
#include <module/mod_light_audio.h>
#include <light_platform.h>
#include "../src/light_audio_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// see the other suites -- a binary linking light_core has to be an application so that
// framework.c's reference to `this_app` resolves. It is never started
static void _test_app_event(const struct light_module *module, uint8_t event, void *arg) {}
static uint8_t _test_app_main(struct light_application *app) { return LF_STATUS_RUN; }
Light_Application_Define(test_audio_pwm, _test_app_event, _test_app_main, &light_audio, &light_core);

static int failures;

#define CHECK(cond, ...) do { \
        if(!(cond)) { \
                printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__); printf("\n"); \
                failures++; \
        } \
} while(0)

// ---------------------------------------------------------------------------------------------
//   the fake PWM peripheral. Records every call in order, because for this driver the ORDER is
// the behaviour -- "released the pin low, then closed it" and "closed it, then released the pin"
// are the same set of calls and only one of them is correct.
// ---------------------------------------------------------------------------------------------
#define MAX_CALLS 32

static struct {
        char calls[MAX_CALLS][48];
        int  count;

        uint8_t  open_pin;
        int      open_calls;
        uint16_t last_wrap;
        uint8_t  last_clkdiv;
        uint32_t last_hz;
        uint16_t last_duty;
        bool     last_release_level;

        // steering: when true, open() reports no PWM, which is what a host build really does
        bool open_fails;
} pwm;

static void _record(const char *fmt, ...)
{
        if(pwm.count >= MAX_CALLS) { pwm.count++; return; }
        va_list args;
        va_start(args, fmt);
        vsnprintf(pwm.calls[pwm.count], sizeof(pwm.calls[0]), fmt, args);
        va_end(args);
        pwm.count++;
}
// the sequence of calls, joined, so a test can assert on order in one readable string
static const char *_sequence(void)
{
        static char buf[MAX_CALLS * 48];
        buf[0] = '\0';
        for(int i = 0; i < pwm.count && i < MAX_CALLS; i++) {
                if(i) strcat(buf, ",");
                strcat(buf, pwm.calls[i]);
        }
        return buf;
}

//   a non-NULL handle that is never dereferenced by the driver -- it only ever passes it back to
// this layer, which is precisely why faking the layer works at all
struct lp_pwm;
static int _fake_block;
#define FAKE_PWM ((struct lp_pwm *)&_fake_block)

struct lp_pwm *__wrap_light_platform_pwm_open(uint8_t pin)
{
        pwm.open_pin = pin;
        pwm.open_calls++;
        _record("open(%u)", pin);
        return pwm.open_fails ? NULL : FAKE_PWM;
}
void __wrap_light_platform_pwm_close(struct lp_pwm *p) { (void)p; _record("close"); }
void __wrap_light_platform_pwm_configure(struct lp_pwm *p, uint16_t wrap, uint8_t clkdiv)
{
        (void)p; pwm.last_wrap = wrap; pwm.last_clkdiv = clkdiv;
        _record("configure(%u,%u)", wrap, clkdiv);
}
uint32_t __wrap_light_platform_pwm_set_frequency(struct lp_pwm *p, uint32_t hz, uint16_t wrap)
{
        (void)p; pwm.last_hz = hz; pwm.last_wrap = wrap;
        _record("freq(%u,%u)", (unsigned)hz, wrap);
        return hz;
}
void __wrap_light_platform_pwm_set_duty(struct lp_pwm *p, uint16_t duty)
{
        (void)p; pwm.last_duty = duty; _record("duty(%u)", duty);
}
void __wrap_light_platform_pwm_set_enabled(struct lp_pwm *p, bool en)
{
        (void)p; _record("enable(%d)", en ? 1 : 0);
}
void __wrap_light_platform_pwm_release_pin(struct lp_pwm *p, bool level)
{
        (void)p; pwm.last_release_level = level; _record("release(%d)", level ? 1 : 0);
}
uint8_t __wrap_light_platform_pwm_get_group(const struct lp_pwm *p) { (void)p; return 3; }

//   each CTest case runs as its own process, so light_audio's statics start clean. This resets
// anyway: running the binary with no argument runs every case in ONE process, and a suite whose
// cases only pass in isolation is worse than no suite
static void setup(void)
{
        memset(&pwm, 0, sizeof(pwm));
        light_core_impl_setup();
        light_audio_init();
}
static struct audio_device *make_device(uint8_t pin)
{
        // create_device opens the pin through _pwm_init, via the object system's add event
        return light_audio_pwm_create_device((uint8_t *)"pwm_dev", pin);
}

// --- 1. opening parks the output silent, which is what stops the first sample clicking ---
static void test_open_parks_the_output_silent(void)
{
        setup();
        struct audio_device *dev = make_device(17);

        CHECK(pwm.open_calls == 1, "expected one open, got %d", pwm.open_calls);
        CHECK(pwm.open_pin == 17, "opened pin %u, expected 17", pwm.open_pin);

        //   configured to the SAMPLE path's wrap and parked at mid-scale, not left disabled.
        // Starting anywhere else means the first sample steps the DC level, which a piezo
        // turns into an audible click
        CHECK(pwm.last_wrap == LIGHT_AUDIO_DUTY_MAX,
              "configured wrap %u, expected %d", pwm.last_wrap, LIGHT_AUDIO_DUTY_MAX);
        CHECK(pwm.last_duty == LIGHT_AUDIO_DUTY_SILENCE,
              "parked at duty %u, expected %d", pwm.last_duty, LIGHT_AUDIO_DUTY_SILENCE);
        CHECK(strstr(_sequence(), "configure") && strstr(_sequence(), "duty"),
              "open should configure and park: %s", _sequence());
        (void)dev;
}

// --- 2. a tone is a 50% square wave at the requested frequency ---
static void test_tone_sets_frequency_and_half_duty(void)
{
        setup();
        struct audio_device *dev = make_device(2);
        pwm.count = 0;   // ignore the open/park calls; this case is about the tone

        light_audio_tone(dev, 440, 0);

        CHECK(pwm.last_hz == 440, "asked for %u Hz, expected 440", (unsigned)pwm.last_hz);
        //   50% of the TONE wrap, not of the sample wrap. A square wave drives a piezo hardest,
        // and amplitude is not meaningfully controllable this way -- volume is the sample path's
        CHECK(pwm.last_duty == pwm.last_wrap / 2,
              "duty %u is not half of wrap %u", pwm.last_duty, pwm.last_wrap);
        CHECK(pwm.last_wrap > 0, "tone should set a non-zero wrap");
}

// --- 3. silence drives the pin LOW rather than merely stopping ---
static void test_tone_zero_drives_the_pin_low(void)
{
        setup();
        struct audio_device *dev = make_device(2);
        light_audio_tone(dev, 440, 0);
        pwm.count = 0;

        light_audio_tone(dev, 0, 0);

        //   a piezo across a floating pin picks up whatever the neighbouring lines are doing and
        // hisses, so 0 Hz has to actively drive the pin -- and must NOT go on to program a
        // frequency of zero, which is what a naive "set the frequency you were given" would do
        CHECK(strstr(_sequence(), "release(0)") != NULL,
              "0 Hz should release the pin low, got: %s", _sequence());
        CHECK(strstr(_sequence(), "freq(0") == NULL,
              "0 Hz should not program a zero frequency, got: %s", _sequence());
}

// --- 4. teardown gives the pin back low, THEN closes -- in that order ---
static void test_destroy_releases_low_then_closes(void)
{
        setup();
        struct audio_device *dev = make_device(2);
        pwm.count = 0;

        // the release path runs from the device's own destructor
        light_object_put(&dev->header);

        const char *seq = _sequence();
        const char *rel = strstr(seq, "release(0)");
        const char *cls = strstr(seq, "close");
        CHECK(rel != NULL, "teardown should release the pin low, got: %s", seq);
        CHECK(cls != NULL, "teardown should close the PWM block, got: %s", seq);
        //   ORDER matters and is the whole point: close() leaves the pin wherever the last duty
        // put it, so releasing after closing would leave a piezo held at whatever level it
        // happened to stop on. Both calls present in the wrong order is a passing set and a
        // failing behaviour
        if(rel && cls) {
                CHECK(rel < cls, "released the pin AFTER closing: %s", seq);
        }
}

// --- 5. no PWM at all is tolerated, which is the real host-build path ---
static void test_absent_pwm_is_tolerated(void)
{
        setup();
        pwm.open_fails = true;          // what light_platform_pwm_open() really does on a host

        struct audio_device *dev = make_device(2);
        pwm.count = 0;

        //   none of these may touch the peripheral, and none may crash. This is not a
        // hypothetical configuration -- it is every host build, and the device is created and
        // used exactly as it would be on hardware
        light_audio_tone(dev, 440, 0);
        light_audio_tone(dev, 0, 0);
        light_audio_stop(dev);
        light_object_put(&dev->header);

        CHECK(pwm.count == 0,
              "a device with no PWM should make no peripheral calls, got: %s", _sequence());
}

// --- 6. the non-streaming fallbacks keep the core's state machine moving ---
static void test_fallbacks_without_pwm_streaming(void)
{
        setup();
        struct audio_device *dev = make_device(2);
        struct audio_driver *drv = light_audio_driver_pwm();

        //   this platform has no LIGHT_PLATFORM_HAS_PWM_STREAM, so submit() reports success and
        // busy() immediately says false: playback completes in silence rather than stalling the
        // core, which would leave a conversion buffer allocated forever
        static const uint8_t duty[4] = { 1, 2, 3, 4 };
        CHECK(drv->submit(dev, duty, 4, 8000),
              "submit should report success on a platform without streaming");
        CHECK(!drv->busy(dev), "busy should be false immediately without streaming");
        drv->stop(dev);         // must not crash
}

static const struct { const char *name; void (*fn)(void); } test_cases[] = {
        { "open_parks_the_output_silent",     test_open_parks_the_output_silent },
        { "tone_sets_frequency_and_half_duty", test_tone_sets_frequency_and_half_duty },
        { "tone_zero_drives_the_pin_low",     test_tone_zero_drives_the_pin_low },
        { "destroy_releases_low_then_closes", test_destroy_releases_low_then_closes },
        { "absent_pwm_is_tolerated",          test_absent_pwm_is_tolerated },
        { "fallbacks_without_pwm_streaming",  test_fallbacks_without_pwm_streaming },
};
#define TEST_CASE_COUNT (sizeof(test_cases) / sizeof(*test_cases))

int main(int argc, char **argv)
{
        if(argc > 1 && strcmp(argv[1], "--list") == 0) {
                for(size_t i = 0; i < TEST_CASE_COUNT; i++) printf("%s\n", test_cases[i].name);
                return 0;
        }
        if(argc > 1) {
                for(size_t i = 0; i < TEST_CASE_COUNT; i++) {
                        if(strcmp(argv[1], test_cases[i].name) != 0) continue;
                        test_cases[i].fn();
                        printf("%s: %s, %d failure(s)\n", test_cases[i].name,
                               failures ? "FAILED" : "PASSED", failures);
                        return failures ? 1 : 0;
                }
                // an unknown name must be an error, not a silent pass -- a typo in
                // CMakeLists.txt would otherwise register a test that always succeeds
                printf("FAIL: no such test case '%s'\n", argv[1]);
                return 2;
        }
        for(size_t i = 0; i < TEST_CASE_COUNT; i++) test_cases[i].fn();
        printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
        return failures ? 1 : 0;
}
