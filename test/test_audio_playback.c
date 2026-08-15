// checks for light_audio's playback lifecycle -- buffer ownership, tone/sample exclusion, and
// the poll loop that ends both.
//
// SEPARATE FROM test_audio_convert.c because it tests a different KIND of thing. That file
// covers pure arithmetic; this one covers state that changes over time, against a fake driver
// standing in for the hardware. The bugs it looks for are the ones that do not show up as a
// wrong sound but as a crash or a leak much later: a conversion buffer freed while DMA is
// still reading out of it, a buffer never freed at all, or a tone left running because the
// sample path reconfigured the slice underneath it.
//
// THE FAKE DRIVER IS THE POINT. light_audio's whole job is sequencing calls to submit(),
// busy(), stop() and tone() correctly; on real hardware that sequence is invisible, and the
// only symptom of getting it wrong is a piezo that occasionally sticks on. Recording the calls
// makes the sequence itself the thing under test.
//
// RUN AS: ctest, or this binary directly. With no argument it runs everything; with a case
// name it runs just that one, which is how CTest registers them individually.
#include <light_audio.h>
#include <module/mod_light_audio.h>
#include <light_platform.h>
//   light_audio_poll_devices() is internal -- the module's periodic task is its only caller in
// the firmware. Reaching past the public header is deliberate: the poll loop is where the
// buffer-lifetime and tone-expiry decisions actually live, so testing only what the public
// header exposes would leave the most consequential function in this module untested
#include "../src/light_audio_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// see test_audio_convert.c -- a binary linking the real library has to be an application, so
// that framework.c's reference to `this_app` resolves. It is never started
static void _test_app_event(const struct light_module *module, uint8_t event, void *arg) {}
static uint8_t _test_app_main(struct light_application *app) { return LF_STATUS_RUN; }
Light_Application_Define(test_audio_playback, _test_app_event, _test_app_main,
                                &light_audio, &light_core);

static int failures;

#define CHECK(cond, ...) do { \
        if(!(cond)) { \
                printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__); printf("\n"); \
                failures++; \
        } \
} while(0)

#define VMAX LIGHT_AUDIO_VOLUME_MAX

// ---------------------------------------------------------------------------------------------
//   the fake driver. Every entry point records that it was called and with what; busy() and
// submit() are steerable, because "the hardware is still going" and "the hardware refused" are
// exactly the two conditions the core has to sequence around and neither can be produced on
// demand from a real device
// ---------------------------------------------------------------------------------------------
static struct {
        int init_calls, submit_calls, busy_calls, stop_calls, tone_calls, destroy_calls;
        const uint8_t *last_duty;
        uint32_t last_count, last_rate, last_hz;
        // steering
        bool busy;
        bool submit_succeeds;
} fake;

static void _fake_init(struct audio_device *dev) { fake.init_calls++; }
static bool _fake_submit(struct audio_device *dev, const uint8_t *duty, uint32_t count,
                         uint32_t sample_rate)
{
        fake.submit_calls++;
        fake.last_duty = duty;
        fake.last_count = count;
        fake.last_rate = sample_rate;
        return fake.submit_succeeds;
}
static bool _fake_busy(struct audio_device *dev) { fake.busy_calls++; return fake.busy; }
static void _fake_stop(struct audio_device *dev) { fake.stop_calls++; }
static void _fake_tone(struct audio_device *dev, uint32_t hz)
{
        fake.tone_calls++;
        fake.last_hz = hz;
}
static void _fake_destroy_context(struct audio_driver_context *ctx)
{
        fake.destroy_calls++;
        light_free(ctx);
}
static struct audio_driver fake_driver;
static struct audio_driver_context *_fake_spawn_context(void)
{
        struct audio_driver_context *ctx = light_alloc(sizeof(struct audio_driver_context));
        ctx->driver = &fake_driver;
        ctx->state = NULL;
        return ctx;
}
static struct audio_driver fake_driver = {
        .name = (const uint8_t *)"fake",
        .spawn_context = _fake_spawn_context,
        .destroy_context = _fake_destroy_context,
        .init_device = _fake_init,
        .submit = _fake_submit,
        .busy = _fake_busy,
        .stop = _fake_stop,
        .tone = _fake_tone
};

//   each CTest case runs as its own process, so the module-level statics in audio.c start
// clean every time. This still resets explicitly rather than relying on that -- running the
// binary with no argument runs every case in ONE process, and a suite whose cases only pass
// in isolation is worse than no suite
static struct audio_device *setup(void)
{
        memset(&fake, 0, sizeof(fake));
        fake.submit_succeeds = true;
        //   the object registry is normally brought up by light_core's module load, which
        // never runs here because the framework is never started. Without it every
        // light_object_alloc() dereferences a NULL registry
        light_core_impl_setup();
        light_audio_init();
        return light_audio_create_device(&fake_driver, (uint8_t *)"fake_dev");
}

static const struct audio_format fmt_u8 = {
        .sample_rate = 8000, .encoding = LIGHT_AUDIO_PCM_U8, .channels = 1
};

// --- 1. every rejection path returns false WITHOUT touching the driver ---
static void test_play_pcm_rejects_bad_arguments(void)
{
        struct audio_device *dev = setup();
        static const uint8_t samples[4] = { 1, 2, 3, 4 };

        CHECK(!light_audio_play_pcm(dev, NULL, 4, fmt_u8), "a NULL sample pointer should be refused");
        CHECK(!light_audio_play_pcm(dev, samples, 0, fmt_u8), "a zero frame count should be refused");

        struct audio_format no_rate = fmt_u8; no_rate.sample_rate = 0;
        CHECK(!light_audio_play_pcm(dev, samples, 4, no_rate), "a zero sample rate should be refused");

        struct audio_format stereo = fmt_u8; stereo.channels = 2;
        CHECK(!light_audio_play_pcm(dev, samples, 4, stereo), "stereo should be refused");

        struct audio_format bad_enc = fmt_u8; bad_enc.encoding = 99;
        CHECK(!light_audio_play_pcm(dev, samples, 4, bad_enc), "an unknown encoding should be refused");

        //   the driver must not have been handed anything. Validating AFTER submitting would
        // still return false while leaving the hardware running on a buffer the core has
        // already disowned
        CHECK(fake.submit_calls == 0, "a rejected format still reached the driver (%d submits)",
              fake.submit_calls);
        CHECK(!light_audio_is_playing(dev), "the device should not be playing after a rejection");
}

// --- 2. a busy device is not interrupted ---
static void test_play_pcm_refuses_while_busy(void)
{
        struct audio_device *dev = setup();
        static const uint8_t samples[4] = { 1, 2, 3, 4 };

        fake.busy = true;
        CHECK(!light_audio_play_pcm(dev, samples, 4, fmt_u8),
              "a busy device should refuse new samples");
        CHECK(fake.submit_calls == 0, "a busy device was handed a buffer anyway");
}

// --- 3. the zero-copy path really is zero-copy ---
static void test_u8_at_full_volume_is_played_in_place(void)
{
        struct audio_device *dev = setup();
        static const uint8_t samples[8] = { 0, 32, 64, 128, 192, 255, 100, 7 };

        CHECK(light_audio_play_pcm(dev, samples, 8, fmt_u8), "U8 at full volume should play");
        //   the driver gets the CALLER's pointer, not a copy. This is the contract that lets a
        // sample stream live in flash and never be copied to RAM, and it is only valid because
        // the U8 full-volume conversion is the identity -- which test_audio_convert.c checks
        CHECK(fake.last_duty == samples,
              "the zero-copy path should submit the source pointer itself");
        CHECK(dev->owned_buffer == NULL,
              "the zero-copy path should not allocate a conversion buffer");
        CHECK(fake.last_count == 8 && fake.last_rate == 8000,
              "submit got count=%u rate=%u, expected 8 and 8000",
              (unsigned)fake.last_count, (unsigned)fake.last_rate);
        CHECK(light_audio_is_playing(dev), "the device should report playing after submit");
}

// --- 4. anything else is converted into a buffer the device owns ---
static void test_conversion_allocates_and_matches(void)
{
        struct audio_device *dev = setup();
        static const int16_t s16[4] = { -32768, 0, 16384, 32767 };

        struct audio_format fmt = { .sample_rate = 16000, .encoding = LIGHT_AUDIO_PCM_S16, .channels = 1 };
        CHECK(light_audio_play_pcm(dev, s16, 4, fmt), "S16 should play");
        CHECK(dev->owned_buffer != NULL, "S16 should have been converted into an owned buffer");
        CHECK(fake.last_duty == dev->owned_buffer,
              "the driver should have been handed the converted buffer");
        for(int i = 0; i < 4; i++) {
                uint8_t want = light_audio_pcm_to_duty(s16[i], LIGHT_AUDIO_PCM_S16, VMAX);
                CHECK(dev->owned_buffer[i] == want, "converted[%d] = %d, expected %d",
                      i, dev->owned_buffer[i], want);
        }

        //   U8 below full volume converts too. Easy to get wrong the other way: the encoding
        // check alone looks sufficient, and skipping the copy here would play the source at
        // full volume no matter what the user set
        struct audio_device *quiet = light_audio_create_device(&fake_driver, (uint8_t *)"quiet_dev");
        light_audio_set_volume(quiet, VMAX / 2);
        static const uint8_t u8[4] = { 0, 64, 192, 255 };
        CHECK(light_audio_play_pcm(quiet, u8, 4, fmt_u8), "U8 at half volume should play");
        CHECK(quiet->owned_buffer != NULL,
              "U8 below full volume must be converted, not played in place");
        CHECK(fake.last_duty != u8, "the attenuated path submitted the source unchanged");
}

// --- 5. a driver that refuses leaves nothing behind ---
static void test_failed_submit_releases_the_buffer(void)
{
        struct audio_device *dev = setup();
        static const int16_t s16[4] = { 100, 200, 300, 400 };
        struct audio_format fmt = { .sample_rate = 16000, .encoding = LIGHT_AUDIO_PCM_S16, .channels = 1 };

        fake.submit_succeeds = false;
        CHECK(!light_audio_play_pcm(dev, s16, 4, fmt), "a refused submit should return false");
        //   the allocation happened before the refusal, so this is a leak unless the failure
        // path frees it -- and it is a leak of one buffer per attempt, on a device with tens
        // of kilobytes of RAM
        CHECK(dev->owned_buffer == NULL, "a refused submit leaked its conversion buffer");
        CHECK(!light_audio_is_playing(dev), "a refused submit should not leave the device playing");
}

// --- 6. the DMA lifetime hazard: the buffer outlives the transfer, not the call ---
static void test_poll_releases_the_buffer_only_when_idle(void)
{
        struct audio_device *dev = setup();
        static const int16_t s16[4] = { 100, 200, 300, 400 };
        struct audio_format fmt = { .sample_rate = 16000, .encoding = LIGHT_AUDIO_PCM_S16, .channels = 1 };

        CHECK(light_audio_play_pcm(dev, s16, 4, fmt), "S16 should play");
        uint8_t *buffer = dev->owned_buffer;
        CHECK(buffer != NULL, "expected a conversion buffer");

        //   while the driver is busy the buffer must stay put. Freeing it here is the bug this
        // case exists for: the DMA is still reading out of it, so the symptom is not a crash
        // at the point of the mistake but noise, or a corrupted allocation much later
        fake.busy = true;
        for(int i = 0; i < 5; i++)
                light_audio_poll_devices();
        CHECK(dev->owned_buffer == buffer, "the buffer was released while the driver was busy");
        CHECK(light_audio_is_playing(dev), "the device stopped reporting playing mid-transfer");
        CHECK(fake.stop_calls == 0, "the driver was stopped mid-transfer");

        // once the transfer is done, the same poll loop has to clean up without being asked
        fake.busy = false;
        light_audio_poll_devices();
        CHECK(dev->owned_buffer == NULL, "the buffer was not released once the driver went idle");
        CHECK(!light_audio_is_playing(dev), "the device still reports playing after the transfer");
        CHECK(fake.stop_calls == 1, "expected exactly one stop, got %d", fake.stop_calls);
}

// --- 7. a tone and a sample stream are the same hardware, configured differently ---
static void test_tone_and_samples_are_mutually_exclusive(void)
{
        struct audio_device *dev = setup();
        static const uint8_t samples[4] = { 1, 2, 3, 4 };

        CHECK(light_audio_play_pcm(dev, samples, 4, fmt_u8), "U8 should play");
        light_audio_tone(dev, 440, 0);
        //   starting a tone reconfigures the slice, so the sample path has to be torn down
        // first -- otherwise the poll loop later frees a buffer for a transfer that a tone
        // has already replaced
        CHECK(fake.stop_calls >= 1, "starting a tone did not stop the sample transfer");
        CHECK(dev->owned_buffer == NULL, "starting a tone did not release the sample buffer");
        CHECK(light_audio_is_playing(dev), "a running tone should count as playing");
        CHECK(fake.last_hz == 440, "the driver got %u Hz, expected 440", (unsigned)fake.last_hz);

        // and 0 Hz is silence rather than a tone at zero, so it must clear the flag too
        light_audio_tone(dev, 0, 0);
        CHECK(!light_audio_is_playing(dev), "0 Hz should not leave the device toning");
}

// --- 8. a tone with a duration ends on the clock, without the caller polling for it ---
static void test_poll_ends_a_tone_on_the_clock(void)
{
        struct audio_device *dev = setup();

        light_audio_tone(dev, 440, 30);
        light_audio_poll_devices();
        CHECK(light_audio_is_playing(dev), "the tone ended before its duration elapsed");

        //   a real sleep rather than an injected clock: light_audio reads
        // light_platform_get_time_since_init() directly, and 40ms of wall time is a cheaper
        // price than a seam through the platform layer for one assertion
        light_platform_sleep_ms(40);
        light_audio_poll_devices();
        CHECK(!light_audio_is_playing(dev), "the tone outlived its duration");
        CHECK(fake.last_hz == 0, "the driver was not told to go silent (last hz = %u)",
              (unsigned)fake.last_hz);
}

// --- 9. duration 0 means "until stopped", not "already expired" ---
static void test_tone_without_duration_runs_until_stopped(void)
{
        struct audio_device *dev = setup();

        light_audio_tone(dev, 880, 0);
        light_platform_sleep_ms(20);
        for(int i = 0; i < 5; i++)
                light_audio_poll_devices();
        //   0 is a sentinel, and the poll loop compares against it rather than treating it as
        // a deadline in the past. Getting that backwards makes an indefinite tone stop on the
        // very next poll, which reads as "the tone never played"
        CHECK(light_audio_is_playing(dev), "an indefinite tone was ended by the poll loop");

        light_audio_stop(dev);
        CHECK(!light_audio_is_playing(dev), "stop did not end the tone");
        CHECK(fake.last_hz == 0, "stop did not silence the driver");
}

// --- 10. volume is clamped where it is set, not where it is used ---
static void test_set_volume_clamps(void)
{
        struct audio_device *dev = setup();

        light_audio_set_volume(dev, VMAX * 2);
        CHECK(dev->volume == VMAX, "an over-range volume should clamp to %d, got %d",
              VMAX, dev->volume);

        //   at exactly max, U8 goes back to the zero-copy path. Worth checking as a pair: a
        // clamp that lands one short leaves the device permanently converting buffers it
        // could have played in place, which is a silent performance regression
        static const uint8_t samples[4] = { 1, 2, 3, 4 };
        CHECK(light_audio_play_pcm(dev, samples, 4, fmt_u8), "U8 should play at clamped volume");
        CHECK(dev->owned_buffer == NULL,
              "a clamped volume should still qualify for the zero-copy path");

        light_audio_set_volume(dev, 0);
        CHECK(dev->volume == 0, "zero volume should be accepted as-is, got %d", dev->volume);
}

static const struct {
        const char *name;
        void (*fn)(void);
} test_cases[] = {
        { "play_pcm_rejects_bad_arguments",   test_play_pcm_rejects_bad_arguments },
        { "play_pcm_refuses_while_busy",      test_play_pcm_refuses_while_busy },
        { "u8_at_full_volume_played_in_place", test_u8_at_full_volume_is_played_in_place },
        { "conversion_allocates_and_matches", test_conversion_allocates_and_matches },
        { "failed_submit_releases_buffer",    test_failed_submit_releases_the_buffer },
        { "poll_releases_buffer_only_when_idle", test_poll_releases_the_buffer_only_when_idle },
        { "tone_and_samples_exclusive",       test_tone_and_samples_are_mutually_exclusive },
        { "poll_ends_tone_on_the_clock",      test_poll_ends_a_tone_on_the_clock },
        { "tone_without_duration",            test_tone_without_duration_runs_until_stopped },
        { "set_volume_clamps",                test_set_volume_clamps },
};
#define TEST_CASE_COUNT (sizeof(test_cases) / sizeof(*test_cases))

int main(int argc, char **argv)
{
        if(argc > 1 && strcmp(argv[1], "--list") == 0) {
                for(size_t i = 0; i < TEST_CASE_COUNT; i++)
                        printf("%s\n", test_cases[i].name);
                return 0;
        }

        if(argc > 1) {
                for(size_t i = 0; i < TEST_CASE_COUNT; i++) {
                        if(strcmp(argv[1], test_cases[i].name) != 0)
                                continue;
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

        for(size_t i = 0; i < TEST_CASE_COUNT; i++)
                test_cases[i].fn();
        printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
        return failures ? 1 : 0;
}
