#include <light_audio.h>
#include <light_platform.h>

#include "light_audio_internal.h"

// the tone path wants the PWM's own frequency to BE the note, which needs a wrap coarse
// enough to leave the divider room to reach down into the audible range. 1000 counts puts the
// achievable range comfortably either side of a piezo's resonance
#define AUDIO_PWM_TONE_WRAP             1000

struct audio_pwm_state {
        uint8_t pin;
        // NULL where the platform has no PWM. every call below tolerates it, so a host build
        // creates the device and simply makes no sound
        struct lp_pwm *pwm;
};

static struct audio_driver_context *_pwm_spawn_context();
static void _pwm_destroy_context(struct audio_driver_context *ctx);
static void _pwm_init(struct audio_device *dev);
static bool _pwm_submit(struct audio_device *dev, const uint8_t *duty, uint32_t count,
                        uint32_t sample_rate);
static bool _pwm_busy(struct audio_device *dev);
static void _pwm_stop(struct audio_device *dev);
static void _pwm_tone(struct audio_device *dev, uint32_t hz);

static struct audio_driver _driver_pwm = {
        .name = "audio.driver:pwm",
        .spawn_context = _pwm_spawn_context,
        .destroy_context = _pwm_destroy_context,
        .init_device = _pwm_init,
        .submit = _pwm_submit,
        .busy = _pwm_busy,
        .stop = _pwm_stop,
        .tone = _pwm_tone
};

struct audio_driver *light_audio_driver_pwm()
{
        return &_driver_pwm;
}

static struct audio_driver_context *_pwm_spawn_context()
{
        struct audio_driver_context *ctx = light_alloc(sizeof(struct audio_driver_context));
        ctx->driver = light_audio_driver_pwm();
        ctx->state = light_alloc(sizeof(struct audio_pwm_state));
        struct audio_pwm_state *state = (struct audio_pwm_state *) ctx->state;
        // light_alloc() isn't zeroed, same as every other driver state in this codebase
        state->pin = 0;
        state->pwm = NULL;
        return ctx;
}

//   the counterpart to _pwm_spawn_context(), called from the device release path
// when the device this context was spawned for is freed. Frees in the reverse of
// the order allocated: the state first, then the context that points at it
static void _pwm_destroy_context(struct audio_driver_context *ctx)
{
        struct audio_pwm_state *state = (struct audio_pwm_state *) ctx->state;
        //   the PWM block is a hardware resource claimed by light_platform_pwm_open(), not
        // memory: freeing the state without closing it leaves the block claimed for the rest
        // of the run, and the next open() of that pin fails.
        //
        //   the pin is handed back driven LOW rather than just closed. close() leaves it
        // wherever the last duty put it, and a piezo across a floating pin is not a silent one
        if(state->pwm) {
                light_platform_pwm_release_pin(state->pwm, false);
                light_platform_pwm_close(state->pwm);
        }
        light_free((void *)ctx->state);
        light_free(ctx);
}

struct audio_device *light_audio_pwm_create_device(uint8_t *name, uint8_t pin)
{
        struct audio_driver_context *ctx = _pwm_spawn_context();
        struct audio_pwm_state *state = (struct audio_pwm_state *) ctx->state;
        state->pin = pin;

        struct audio_device *dev = light_object_alloc(sizeof(struct audio_device));
        return light_audio_init_device(dev, ctx, name);
}

static void _pwm_init(struct audio_device *dev)
{
        struct audio_pwm_state *state = (struct audio_pwm_state *) dev->driver_ctx->state;
        state->pwm = light_platform_pwm_open(state->pin);

        // the group is logged because PWM blocks are SHARED between pins: a buzzer landing on
        // the backlight's block would fight it over period and duty, and the symptom -- the
        // backlight flickering in time with audio -- points nowhere near the cause. printing
        // it makes a clash on a new board visible instead of mysterious
        light_info("pwm audio '%s' on pin %d (group %d)",
                        dev->header.id, state->pin, light_platform_pwm_get_group(state->pwm));

        // parked silent rather than left disabled: starting from the same configuration the
        // sample path uses means the first sample does not step the DC level, and a piezo
        // turns a DC step into an audible click
        if(state->pwm) {
                light_platform_pwm_configure(state->pwm, LIGHT_AUDIO_DUTY_MAX, 1);
                light_platform_pwm_set_duty(state->pwm, LIGHT_AUDIO_DUTY_SILENCE);
        }
}

static bool _pwm_submit(struct audio_device *dev, const uint8_t *duty, uint32_t count,
                        uint32_t sample_rate)
{
        struct audio_pwm_state *state = (struct audio_pwm_state *) dev->driver_ctx->state;
#if LIGHT_PLATFORM_HAS_PWM_STREAM
        // the platform puts the PWM into DAC mode itself -- the supersonic carrier that
        // implies is inherent to the mechanism rather than something this driver should be
        // choosing, and a preceding tone will have configured it away
        return light_platform_pwm_stream_start(state->pwm, duty, count, sample_rate);
#else
        // no streaming on this platform. reporting success keeps the core's state machine
        // moving -- busy() immediately says false, so playback simply completes in silence
        (void)state; (void)duty; (void)count; (void)sample_rate;
        return true;
#endif
}

static bool _pwm_busy(struct audio_device *dev)
{
        struct audio_pwm_state *state = (struct audio_pwm_state *) dev->driver_ctx->state;
#if LIGHT_PLATFORM_HAS_PWM_STREAM
        return light_platform_pwm_stream_busy(state->pwm);
#else
        (void)state;
        return false;
#endif
}

static void _pwm_stop(struct audio_device *dev)
{
        struct audio_pwm_state *state = (struct audio_pwm_state *) dev->driver_ctx->state;
#if LIGHT_PLATFORM_HAS_PWM_STREAM
        light_platform_pwm_stream_stop(state->pwm);
#else
        (void)state;
#endif
}

static void _pwm_tone(struct audio_device *dev, uint32_t hz)
{
        struct audio_pwm_state *state = (struct audio_pwm_state *) dev->driver_ctx->state;
        if(!state->pwm)
                return;

        if(!hz) {
                // driven low rather than left floating: a piezo across a floating pin picks up
                // whatever the neighbouring lines are doing and hisses
                light_platform_pwm_release_pin(state->pwm, false);
                return;
        }

        light_platform_pwm_set_frequency(state->pwm, hz, AUDIO_PWM_TONE_WRAP);
        // 50% duty: a square wave drives a piezo hardest, and amplitude is not meaningfully
        // controllable this way -- volume applies to the sample path
        light_platform_pwm_set_duty(state->pwm, AUDIO_PWM_TONE_WRAP / 2);
}
