// checks for light_audio's PCM -> duty conversion.
//
// pure arithmetic and needs no hardware, which matters because every way of getting this
// wrong is AUDIBLE but none of them are visible in the code: a missing re-centring, a clamp
// that wraps, attenuating towards zero instead of towards silence, or a divide where the code
// shifts all compile cleanly and all sound broken. Testing it on hardware means listening to
// a piezo and guessing.
//
// RUN AS: ctest, or this binary directly. With no argument it runs everything; with a case
// name it runs just that one, which is how CTest registers them individually.
//
// See test/mutants.ps1 for how the coverage here was established -- several of these cases
// exist only because a mutant survived the first version of this file.
#include <light_audio.h>
#include <module/mod_light_audio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// light_core's framework.c refers to `this_app`, which only an application defines -- so a
// test binary linking the real library has to be one. it is never started; these exist to
// satisfy the linker and to keep the test compiling against the same headers the firmware
// does rather than a stubbed stand-in
static void _test_app_event(const struct light_module *module, uint8_t event, void *arg) {}
static uint8_t _test_app_main(struct light_application *app) { return LF_STATUS_RUN; }
Light_Application_Define(test_audio_convert, _test_app_event, _test_app_main,
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
#define SIL  LIGHT_AUDIO_DUTY_SILENCE

// --- 1. the fixed points that define the mapping ---
static void test_anchors(void)
{
        // silence must land on mid-scale. landing on 0 instead is the classic signed->unsigned
        // slip, and it does not sound quiet -- it pins the transducer to one rail
        CHECK(light_audio_pcm_to_duty(0, LIGHT_AUDIO_PCM_S16, VMAX) == SIL,
              "S16 silence -> %d, expected %d",
              light_audio_pcm_to_duty(0, LIGHT_AUDIO_PCM_S16, VMAX), SIL);
        CHECK(light_audio_pcm_to_duty(128, LIGHT_AUDIO_PCM_U8, VMAX) == SIL,
              "U8 silence -> %d, expected %d",
              light_audio_pcm_to_duty(128, LIGHT_AUDIO_PCM_U8, VMAX), SIL);

        // full scale reaches the ends without wrapping past them
        CHECK(light_audio_pcm_to_duty(-32768, LIGHT_AUDIO_PCM_S16, VMAX) == 0,
              "S16 -32768 -> %d, expected 0",
              light_audio_pcm_to_duty(-32768, LIGHT_AUDIO_PCM_S16, VMAX));
        CHECK(light_audio_pcm_to_duty(32767, LIGHT_AUDIO_PCM_S16, VMAX) == LIGHT_AUDIO_DUTY_MAX,
              "S16 32767 -> %d, expected %d",
              light_audio_pcm_to_duty(32767, LIGHT_AUDIO_PCM_S16, VMAX), LIGHT_AUDIO_DUTY_MAX);
        CHECK(light_audio_pcm_to_duty(0, LIGHT_AUDIO_PCM_U8, VMAX) == 0,
              "U8 0 -> %d, expected 0", light_audio_pcm_to_duty(0, LIGHT_AUDIO_PCM_U8, VMAX));
        CHECK(light_audio_pcm_to_duty(255, LIGHT_AUDIO_PCM_U8, VMAX) == LIGHT_AUDIO_DUTY_MAX,
              "U8 255 -> %d, expected %d",
              light_audio_pcm_to_duty(255, LIGHT_AUDIO_PCM_U8, VMAX), LIGHT_AUDIO_DUTY_MAX);
}

// --- 2. U8 at full volume is the identity, which is what the zero-copy path assumes ---
static void test_u8_passthrough_is_identity(void)
{
        for(int u = 0; u <= 255; u++) {
                uint8_t out = light_audio_pcm_to_duty(u, LIGHT_AUDIO_PCM_U8, VMAX);
                if(out != u) {
                        CHECK(0, "U8 passthrough not identity at %d -> %d "
                                 "(the zero-copy path in light_audio_play_pcm is invalid)",
                              u, out);
                        return;
                }
        }
}

// --- 3. monotonic: louder input never produces quieter output ---
static void test_monotonic(void)
{
        int prev = -1;
        for(int s = -32768; s <= 32767; s += 7) {
                int d = light_audio_pcm_to_duty(s, LIGHT_AUDIO_PCM_S16, VMAX);
                CHECK(d >= prev, "S16 mapping not monotonic at %d: %d after %d", s, d, prev);
                if(d < prev) return;
                prev = d;
        }
}

// --- 4. volume attenuates towards SILENCE, not towards zero ---
static void test_volume_collapses_to_silence(void)
{
        // volume 0 must be flat mid-scale for EVERY input. if attenuation ran towards zero
        // this would be 0 for every input instead -- equally "silent" in the sense of
        // constant, but a full-scale DC step away from where the waveform was, which a piezo
        // renders as a click every time the volume is dropped
        int inputs[] = { -32768, -20000, -1, 0, 1, 20000, 32767 };
        for(size_t i = 0; i < sizeof(inputs)/sizeof(*inputs); i++) {
                uint8_t d = light_audio_pcm_to_duty(inputs[i], LIGHT_AUDIO_PCM_S16, 0);
                CHECK(d == SIL, "volume 0 on input %d -> %d, expected silence %d",
                      inputs[i], d, SIL);
        }

        // and the range must shrink monotonically about the midpoint as volume falls
        int prev_span = 1 << 30;
        for(int v = VMAX; v >= 0; v -= 50) {
                int lo = light_audio_pcm_to_duty(-32768, LIGHT_AUDIO_PCM_S16, v);
                int hi = light_audio_pcm_to_duty(32767, LIGHT_AUDIO_PCM_S16, v);
                int span = hi - lo;
                CHECK(span <= prev_span, "volume %d widened the span to %d from %d",
                      v, span, prev_span);
                // the midpoint must not drift as the volume changes -- that drift IS the DC
                // step this whole arrangement exists to avoid
                int mid = (hi + lo) / 2;
                CHECK(mid >= SIL - 1 && mid <= SIL,
                      "volume %d moved the midpoint to %d, expected about %d", v, mid, SIL);
                prev_span = span;
        }
}

// --- 5. half volume is genuinely half the excursion ---
static void test_half_volume(void)
{
        int full_hi = light_audio_pcm_to_duty(32767, LIGHT_AUDIO_PCM_S16, VMAX) - SIL;
        int half_hi = light_audio_pcm_to_duty(32767, LIGHT_AUDIO_PCM_S16, VMAX / 2) - SIL;
        CHECK(half_hi * 2 >= full_hi - 2 && half_hi * 2 <= full_hi + 2,
              "half volume excursion %d is not half of %d", half_hi, full_hi);
}

// --- 6. out-of-range input is clamped, not wrapped ---
static void test_clamping(void)
{
        // an encoder that hands over a value outside the 16-bit range must not wrap the duty
        // register past the PWM wrap value -- that would alias to a completely wrong level
        CHECK(light_audio_pcm_to_duty(100000, LIGHT_AUDIO_PCM_S16, VMAX) == LIGHT_AUDIO_DUTY_MAX,
              "over-range positive wrapped to %d",
              light_audio_pcm_to_duty(100000, LIGHT_AUDIO_PCM_S16, VMAX));
        CHECK(light_audio_pcm_to_duty(-100000, LIGHT_AUDIO_PCM_S16, VMAX) == 0,
              "over-range negative wrapped to %d",
              light_audio_pcm_to_duty(-100000, LIGHT_AUDIO_PCM_S16, VMAX));
        // and volume above the maximum must not amplify
        CHECK(light_audio_pcm_to_duty(32767, LIGHT_AUDIO_PCM_S16, 5000) == LIGHT_AUDIO_DUTY_MAX,
              "over-range volume produced %d",
              light_audio_pcm_to_duty(32767, LIGHT_AUDIO_PCM_S16, 5000));
}

// --- 7. a full-scale square wave stays symmetric about silence ---
static void test_symmetry(void)
{
        // asymmetry about the midpoint is a DC component, which on a piezo is wasted power
        // and on a speaker is cone offset. one code of asymmetry is inherent to two's
        // complement (-32768 has no positive twin); more than that is a bug
        for(int v = 0; v <= VMAX; v += 100) {
                int lo = SIL - light_audio_pcm_to_duty(-32768, LIGHT_AUDIO_PCM_S16, v);
                int hi = light_audio_pcm_to_duty(32767, LIGHT_AUDIO_PCM_S16, v) - SIL;
                CHECK(lo - hi <= 1 && hi - lo <= 1,
                      "volume %d: excursion %d below silence vs %d above", v, lo, hi);
        }
}

// --- 8. no dead zone: every output code covers the same span of input codes ---
static void test_uniform_code_spans(void)
{
        // this is what separates an arithmetic shift from a divide. a divide truncates
        // TOWARDS ZERO, so the codes either side of silence collapse onto the same output and
        // twice as many inputs map to silence as to anything else. that is a dead zone right
        // where a waveform crosses zero -- crossover distortion, loudest on quiet material,
        // and completely invisible in the anchors and monotonicity checks above
        int count[256];
        memset(count, 0, sizeof(count));
        for(int s = -32768; s <= 32767; s++)
                count[light_audio_pcm_to_duty(s, LIGHT_AUDIO_PCM_S16, VMAX)]++;

        for(int d = 0; d < 256; d++) {
                if(count[d] != 256) {
                        CHECK(0, "duty %d covers %d input codes, expected 256 "
                                 "(uneven spans mean a dead zone -- shift vs divide)",
                              d, count[d]);
                        return;
                }
        }
}

// --- 9. volume above the maximum behaves exactly as the maximum ---
static void test_over_range_volume_is_clamped(void)
{
        // without the volume clamp this amplifies mid-range samples rather than being a
        // no-op: the output clamp only catches the ones that reach the rails, so the
        // distortion sits in the middle of the waveform where the clamp never fires
        for(int s = -32768; s <= 32767; s += 13) {
                uint8_t at_max = light_audio_pcm_to_duty(s, LIGHT_AUDIO_PCM_S16, VMAX);
                uint8_t over = light_audio_pcm_to_duty(s, LIGHT_AUDIO_PCM_S16, VMAX * 5);
                if(at_max != over) {
                        CHECK(0, "sample %d: volume %d gave %d but volume %d gave %d",
                              s, VMAX, at_max, VMAX * 5, over);
                        return;
                }
        }
}

// --- 10. an absurd input cannot overflow the attenuation multiply ---
static void test_extreme_input(void)
{
        // the input clamp is what keeps `centred` inside a byte before it is multiplied by
        // the volume. without it a large sample overflows the int32 multiply and the result
        // is anything at all -- including a duty that looks perfectly reasonable
        CHECK(light_audio_pcm_to_duty(2147483647, LIGHT_AUDIO_PCM_S16, VMAX) == LIGHT_AUDIO_DUTY_MAX,
              "INT32_MAX -> %d, expected %d",
              light_audio_pcm_to_duty(2147483647, LIGHT_AUDIO_PCM_S16, VMAX), LIGHT_AUDIO_DUTY_MAX);
        CHECK(light_audio_pcm_to_duty(-2147483647, LIGHT_AUDIO_PCM_S16, VMAX) == 0,
              "-INT32_MAX -> %d, expected 0",
              light_audio_pcm_to_duty(-2147483647, LIGHT_AUDIO_PCM_S16, VMAX));
}

// the cases CTest registers one at a time. duplicated in test/CMakeLists.txt by name rather
// than discovered, because a discovery step that silently found nothing would report a clean
// run having tested precisely zero things. `--list` is there so the two can be compared
static const struct {
        const char *name;
        void (*fn)(void);
} test_cases[] = {
        { "anchors",                 test_anchors },
        { "u8_passthrough_identity", test_u8_passthrough_is_identity },
        { "monotonic",               test_monotonic },
        { "volume_to_silence",       test_volume_collapses_to_silence },
        { "half_volume",             test_half_volume },
        { "clamping",                test_clamping },
        { "symmetry",                test_symmetry },
        { "uniform_code_spans",      test_uniform_code_spans },
        { "over_range_volume",       test_over_range_volume_is_clamped },
        { "extreme_input",           test_extreme_input },
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
                // an unknown name is an error rather than a silent pass: a typo in
                // CMakeLists.txt would otherwise register a test that can never fail
                printf("FAIL: no such test case '%s'\n", argv[1]);
                return 2;
        }

        for(size_t i = 0; i < TEST_CASE_COUNT; i++)
                test_cases[i].fn();
        printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
        return failures ? 1 : 0;
}
