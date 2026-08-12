#include <light_audio.h>
#include <light_platform.h>

#include "light_audio_internal.h"

#if(LIGHT_SYSTEM == SYSTEM_PICO_SDK)
#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pwm.h>
#endif

// the sample path runs the slice as a DAC carrier: undivided clock, wrapping at the duty
// maximum. on a 150MHz RP2350 that is 150e6/256 = 586kHz, far above anything audible, so the
// carrier itself is inaudible and only the modulated duty is heard. the wrap is exactly
// LIGHT_AUDIO_DUTY_MAX so a duty value maps straight onto the compare register with no
// rescaling, the same trick light_backlight plays with its level scale
#define AUDIO_PWM_DAC_CLKDIV            1
#define AUDIO_PWM_DAC_WRAP              LIGHT_AUDIO_DUTY_MAX

// the tone path wants the slice's own frequency to BE the tone, which for anything audible
// needs the clock divided a long way down. a wrap of 1000 with a divider chosen per-tone puts
// the achievable range comfortably either side of a piezo's resonance
#define AUDIO_PWM_TONE_WRAP             1000

struct audio_pwm_state {
        uint8_t pin;
        uint8_t slice;
        uint8_t channel;
        int dma_channel;
        // the DMA pacing timer that clocks samples out at the sample rate. -1 until the
        // first sample transfer claims one; tones never need it
        int dma_timer;
};

static struct audio_driver_context *_pwm_spawn_context();
static void _pwm_init(struct audio_device *dev);
static bool _pwm_submit(struct audio_device *dev, const uint8_t *duty, uint32_t count,
                        uint32_t sample_rate);
static bool _pwm_busy(struct audio_device *dev);
static void _pwm_stop(struct audio_device *dev);
static void _pwm_tone(struct audio_device *dev, uint32_t hz);

static struct audio_driver _driver_pwm = {
        .name = "audio.driver:pwm",
        .spawn_context = _pwm_spawn_context,
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
        state->slice = 0;
        state->channel = 0;
        state->dma_channel = -1;
        state->dma_timer = -1;
        return ctx;
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
#if(LIGHT_SYSTEM == SYSTEM_PICO_SDK)
        gpio_set_function(state->pin, GPIO_FUNC_PWM);
        state->slice = (uint8_t)pwm_gpio_to_slice_num(state->pin);
        state->channel = (uint8_t)pwm_gpio_to_channel(state->pin);
        state->dma_channel = dma_claim_unused_channel(true);

        // the slice is logged because PWM slices are SHARED between pin pairs: a buzzer that
        // lands on the backlight's slice would fight it over wrap and clkdiv, and the symptom
        // -- the backlight flickering in time with audio -- points nowhere near the cause.
        // printing it here makes the clash visible on a new board instead of mysterious
        light_info("pwm audio '%s' on pin %d (slice %d, channel %s), dma channel %d",
                        dev->header.id, state->pin, state->slice,
                        state->channel == PWM_CHAN_A ? "A" : "B", state->dma_channel);

        // parked silent: mid-scale duty on the DAC carrier, which is 0V average across the
        // transducer. leaving the slice disabled would work too, but starting from the same
        // configuration the sample path uses means the first sample does not step the DC
        // level, and a piezo turns a DC step into an audible click
        pwm_config cfg = pwm_get_default_config();
        pwm_config_set_clkdiv_int(&cfg, AUDIO_PWM_DAC_CLKDIV);
        pwm_config_set_wrap(&cfg, AUDIO_PWM_DAC_WRAP);
        pwm_init(state->slice, &cfg, true);
        pwm_set_chan_level(state->slice, state->channel, LIGHT_AUDIO_DUTY_SILENCE);
#else
        light_info("pwm audio '%s' on pin %d (host build: no output)", dev->header.id, state->pin);
#endif
}

#if(LIGHT_SYSTEM == SYSTEM_PICO_SDK)
// the compare register half this pin's channel drives. cc is one 32-bit register holding both
// channels, A in the low half, so the DMA writes 8 bits at the low byte of the right half --
// valid precisely because the wrap is 255, so every duty value fits in a byte
static volatile uint8_t *_cc_byte(struct audio_pwm_state *state)
{
        volatile uint8_t *cc = (volatile uint8_t *)&pwm_hw->slice[state->slice].cc;
        return cc + (state->channel == PWM_CHAN_B ? 2 : 0);
}
#endif

static bool _pwm_submit(struct audio_device *dev, const uint8_t *duty, uint32_t count,
                        uint32_t sample_rate)
{
        struct audio_pwm_state *state = (struct audio_pwm_state *) dev->driver_ctx->state;
#if(LIGHT_SYSTEM == SYSTEM_PICO_SDK)
        if(state->dma_channel < 0 || dma_channel_is_busy(state->dma_channel))
                return false;

        // back to the DAC configuration, which a preceding tone will have changed
        pwm_set_enabled(state->slice, false);
        pwm_config cfg = pwm_get_default_config();
        pwm_config_set_clkdiv_int(&cfg, AUDIO_PWM_DAC_CLKDIV);
        pwm_config_set_wrap(&cfg, AUDIO_PWM_DAC_WRAP);
        pwm_init(state->slice, &cfg, true);

        // a DMA pacing timer rather than a second PWM slice or a sample-rate interrupt. it
        // issues a DREQ at sys_clk * X / Y for 16-bit X and Y, which gives an arbitrary rate
        // off the system clock for the cost of one timer and no CPU at all -- at 22050Hz that
        // is tens of thousands of interrupts a second not being taken
        if(state->dma_timer < 0) {
                state->dma_timer = dma_claim_unused_timer(false);
                if(state->dma_timer < 0) {
                        light_error("light_audio: no DMA pacing timer available","");
                        return false;
                }
        }
        uint32_t sys_hz = clock_get_hz(clk_sys);
        // X/Y with X == 1 keeps Y in range for every rate a buzzer will ever be asked for
        // (Y is 16-bit, so this covers anything above sys_clk/65535, about 2.3kHz on a
        // 150MHz part) and keeps the error to the rounding of a single divide
        uint32_t divisor = (sys_hz + sample_rate / 2) / sample_rate;
        if(divisor > 0xFFFF) divisor = 0xFFFF;
        if(divisor < 1) divisor = 1;
        dma_timer_set_fraction(state->dma_timer, 1, (uint16_t)divisor);

        dma_channel_config c = dma_channel_get_default_config(state->dma_channel);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, dma_get_timer_dreq(state->dma_timer));
        dma_channel_configure(state->dma_channel, &c,
                        _cc_byte(state),        // one PWM compare byte, written repeatedly
                        duty,
                        count,
                        true);
        return true;
#else
        (void)duty; (void)count; (void)sample_rate; (void)state;
        return true;
#endif
}

static bool _pwm_busy(struct audio_device *dev)
{
        struct audio_pwm_state *state = (struct audio_pwm_state *) dev->driver_ctx->state;
#if(LIGHT_SYSTEM == SYSTEM_PICO_SDK)
        return state->dma_channel >= 0 && dma_channel_is_busy(state->dma_channel);
#else
        (void)state;
        return false;
#endif
}

static void _pwm_stop(struct audio_device *dev)
{
        struct audio_pwm_state *state = (struct audio_pwm_state *) dev->driver_ctx->state;
#if(LIGHT_SYSTEM == SYSTEM_PICO_SDK)
        if(state->dma_channel >= 0 && dma_channel_is_busy(state->dma_channel))
                dma_channel_abort(state->dma_channel);
        // parked at silence rather than switched off, so stopping mid-sample settles the
        // transducer at 0V average instead of leaving it held at whatever duty it reached
        pwm_set_chan_level(state->slice, state->channel, LIGHT_AUDIO_DUTY_SILENCE);
#else
        (void)state;
#endif
}

static void _pwm_tone(struct audio_device *dev, uint32_t hz)
{
        struct audio_pwm_state *state = (struct audio_pwm_state *) dev->driver_ctx->state;
#if(LIGHT_SYSTEM == SYSTEM_PICO_SDK)
        if(!hz) {
                pwm_set_enabled(state->slice, false);
                gpio_set_function(state->pin, GPIO_FUNC_SIO);
                gpio_set_dir(state->pin, GPIO_OUT);
                // driven low rather than left floating: a piezo across a floating pin picks
                // up whatever the neighbouring lines are doing and hisses
                gpio_put(state->pin, 0);
                return;
        }

        gpio_set_function(state->pin, GPIO_FUNC_PWM);
        // f_pwm = sys_clk / (clkdiv * (wrap + 1)), solved for an integer clkdiv at a fixed
        // wrap. the integer divider variant, not the float one, for the same reason
        // light_backlight uses it: no floating point in driver paths, so an FPU-less RP2040
        // pays the same as an RP2350
        uint32_t sys_hz = clock_get_hz(clk_sys);
        uint32_t divisor = sys_hz / (hz * (AUDIO_PWM_TONE_WRAP + 1));
        if(divisor < 1) divisor = 1;
        if(divisor > 255) divisor = 255;

        pwm_set_enabled(state->slice, false);
        pwm_config cfg = pwm_get_default_config();
        pwm_config_set_clkdiv_int(&cfg, divisor);
        pwm_config_set_wrap(&cfg, AUDIO_PWM_TONE_WRAP);
        pwm_init(state->slice, &cfg, true);
        // 50% duty: a square wave is what drives a piezo hardest, and amplitude is not
        // meaningfully controllable this way -- volume applies to the sample path
        pwm_set_chan_level(state->slice, state->channel, AUDIO_PWM_TONE_WRAP / 2);
#else
        (void)state; (void)hz;
#endif
}
