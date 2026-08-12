#ifndef _LIGHT_AUDIO_H
#define _LIGHT_AUDIO_H

#include <light.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#define ID_AUDIO_DEVICE_ROOT                    "light_audio:device_root"
#define ID_AUDIO_DEVICE                         "light_audio:device"

#define LIGHT_AUDIO_MAX_DEVICES                 2

// volume is per-mille, 0..1000, matching light_backlight's level scale so the two read the
// same way at call sites. it attenuates towards SILENCE, which for unsigned duty output means
// towards mid-scale rather than towards zero -- see light_audio_pcm_to_duty()
#define LIGHT_AUDIO_VOLUME_MAX                  1000

// every driver in this library outputs 8-bit unsigned duty, so mid-scale is silence and the
// conversion below is the only place a source format is interpreted
#define LIGHT_AUDIO_DUTY_MAX                    255
#define LIGHT_AUDIO_DUTY_SILENCE                128

// source sample encodings. only uncompressed PCM for now; this field is the seam a
// compressed format would slot into, without the call signature changing
#define LIGHT_AUDIO_PCM_U8                      0
#define LIGHT_AUDIO_PCM_S16                     1

struct audio_format {
        uint32_t sample_rate;
        uint8_t encoding;
        // 1 only for now. one transducer, and a second channel needs a second PWM slice and a
        // second DMA -- the field exists so adding that later doesn't change this call
        uint8_t channels;
};

struct audio_device;
struct audio_driver
{
        const uint8_t *name;
        struct audio_driver_context *(*spawn_context)();
        void (*init_device)(struct audio_device *);
        // hands over a block of 8-bit unsigned duty values to play at `sample_rate`. the
        // buffer must stay valid until busy() goes false -- the driver does not copy it,
        // because the whole point of the zero-copy path is that a source already in this
        // format is played straight out of flash
        bool (*submit)(struct audio_device *, const uint8_t *duty, uint32_t count,
                       uint32_t sample_rate);
        bool (*busy)(struct audio_device *);
        void (*stop)(struct audio_device *);
        // a continuous square wave at `hz`, or silence for 0. this is a completely different
        // configuration of the same hardware from the sample path above -- the carrier
        // frequency IS the tone, rather than being a supersonic carrier whose duty is
        // modulated -- which is why it is a driver entry point and not something the core
        // synthesises into samples
        void (*tone)(struct audio_device *, uint32_t hz);
};
struct audio_driver_context
{
        const struct audio_driver *driver;
        const void *state;
};

struct audio_device {
        struct light_object header;
        uint8_t device_id;
        uint16_t volume;
        struct audio_driver_context *driver_ctx;

        // --- playback state, advanced from light_audio_poll_devices() ---
        // the converted buffer, owned by this device and freed when playback ends. NULL when
        // the source needed no conversion and is being played where it lies
        uint8_t *owned_buffer;
        bool playing;
        // tone rather than samples. tracked separately because a tone ends on a clock rather
        // than on the DMA running out
        bool toning;
        uint32_t tone_end_ms;
};
struct audio_device_root {
        struct light_object header;
        struct audio_device *device[LIGHT_AUDIO_MAX_DEVICES];
};

#define to_audio_device_root(ptr) container_of(ptr, struct audio_device_root, header)
#define to_audio_device(ptr) container_of(ptr, struct audio_device, header)

extern void light_audio_init();
extern struct audio_device_root *light_audio_device_get_root();
extern struct audio_device *light_audio_create_device(struct audio_driver *driver,
                                                uint8_t *format, ...);
extern struct audio_device *light_audio_create_device_va(struct audio_driver *driver,
                                                uint8_t *format, va_list args);
extern struct audio_device *light_audio_init_device(
                                                struct audio_device *dev,
                                                struct audio_driver_context *driver_ctx,
                                                uint8_t *format, ...);
extern struct audio_device *light_audio_init_device_va(
                                                struct audio_device *dev,
                                                struct audio_driver_context *driver_ctx,
                                                uint8_t *format, va_list args);
extern void light_audio_command_init(struct audio_device *dev);

// 0..LIGHT_AUDIO_VOLUME_MAX. takes effect on the NEXT thing played -- a sample buffer is
// converted once, up front, so changing the volume mid-playback would mean re-converting
// what has already been handed to DMA
extern void light_audio_set_volume(struct audio_device *dev, uint16_t volume);

// plays `frame_count` samples of uncompressed PCM, converting to the driver's duty
// representation first. returns false if the device is busy, the format is unsupported, or
// the conversion buffer could not be allocated.
//
// `samples` must stay valid for the duration of playback when no conversion happens (U8 at
// full volume plays straight out of the caller's memory, which is what lets a pre-baked asset
// sit in flash and cost no RAM). when a conversion does happen the caller's buffer is no
// longer referenced once this returns
extern bool light_audio_play_pcm(struct audio_device *dev, const void *samples,
                                uint32_t frame_count, struct audio_format format);

// a square wave at `hz` for `duration_ms`, or until stopped if the duration is 0. this is
// what a piezo buzzer is actually good at -- it is a sharply resonant device, so a tone near
// its resonance is far louder than PCM through the same pin will ever be
extern void light_audio_tone(struct audio_device *dev, uint32_t hz, uint32_t duration_ms);

extern void light_audio_stop(struct audio_device *dev);
extern bool light_audio_is_playing(const struct audio_device *dev);

// converts one PCM sample to 8-bit unsigned duty at `volume`. exposed rather than kept
// private because it is the only place a source format is interpreted, and it is the part
// most worth testing on its own -- every way of getting this wrong is audible but none of
// them are visible in the code.
//
// volume attenuates towards LIGHT_AUDIO_DUTY_SILENCE, not towards zero. attenuating towards
// zero would slew the DC level as the volume changed, and a piezo renders a DC step as a
// click
extern uint8_t light_audio_pcm_to_duty(int32_t sample, uint8_t encoding, uint16_t volume);

// the driver for a passive buzzer or piezo on a PWM-capable pin. in-repo rather than its own
// module for the same reason light_backlight's PWM driver and light_button's GPIO driver are:
// there is no vendor part here to describe, only a pin
extern struct audio_driver *light_audio_driver_pwm();
extern struct audio_device *light_audio_pwm_create_device(uint8_t *name, uint8_t pin);

#endif
