#include <light_audio.h>
#include <light_platform.h>

#include "light_audio_internal.h"

static void _device_root_child_add(struct light_object *obj, struct light_object *child)
{
        struct audio_device_root *root = to_audio_device_root(obj);
        struct audio_device *dev = to_audio_device(child);
        root->device[dev->device_id] = dev;
}
static void _device_release(struct light_object *obj)
{
        struct audio_device *dev = to_audio_device(obj);
        //   the driver context was spawned for this device alone, so it goes with it. Without
        // this the device is reclaimed and its context -- plus whatever driver state hangs
        // off it -- is not, a leak that only becomes visible once teardown is exercised
        if(dev->driver_ctx && dev->driver_ctx->driver->destroy_context)
                dev->driver_ctx->driver->destroy_context(dev->driver_ctx);
        light_free(dev);
}
static void _device_add(struct light_object *obj, struct light_object *parent) {
        struct audio_device *dev = to_audio_device(obj);
        light_debug("name=%s", dev->header.id);
        light_audio_command_init(dev);
}
// singleton container object for audio_device objects
static struct lobj_type ltype_audio_device_root = (struct lobj_type) {
        .id = ID_AUDIO_DEVICE_ROOT,
        .release = NULL,
        .evt_child_add = _device_root_child_add
};
static struct lobj_type ltype_audio_device = (struct lobj_type) {
        .id = ID_AUDIO_DEVICE,
        .release = _device_release,
        .evt_add = _device_add
};
static struct audio_device_root device_root;

static volatile uint16_t next_device_id;

void light_audio_init()
{
        next_device_id = 0;
        light_object_init_static(&device_root.header, &ltype_audio_device_root);
        light_object_add(&device_root.header, NULL, "root_device");
}
struct audio_device_root *light_audio_device_get_root()
{
        return &device_root;
}
struct audio_device *light_audio_create_device(struct audio_driver *driver,
                                                uint8_t *format, ...)
{
        va_list vargs;

        va_start(vargs, format);
        struct audio_device *dev = light_audio_create_device_va(driver, format, vargs);
        va_end(vargs);
        return dev;
}
struct audio_device *light_audio_create_device_va(struct audio_driver *driver,
                                                uint8_t *format, va_list args)
{
        struct audio_device *dev = light_object_alloc(sizeof(struct audio_device));
        struct audio_driver_context *driver_ctx = driver->spawn_context();

        return light_audio_init_device_va(dev, driver_ctx, format, args);
}
struct audio_device *light_audio_init_device(
                struct audio_device *dev,
                struct audio_driver_context *driver_ctx,
                uint8_t *format, ...)
{
        va_list vargs;

        va_start(vargs, format);
        struct audio_device *out = light_audio_init_device_va(dev, driver_ctx, format, vargs);
        va_end(vargs);
        return out;
}
struct audio_device *light_audio_init_device_va(
                struct audio_device *dev,
                struct audio_driver_context *driver_ctx,
                uint8_t *format, va_list args)
{
        light_trace("(driver=%s)", driver_ctx->driver->name);
        // TODO: this should be an ASSERT statement
        if(next_device_id >= LIGHT_AUDIO_MAX_DEVICES) {
                light_error("could not create new device: max devices reached (%d)", next_device_id);
                return NULL;
        }
        uint8_t device_id = next_device_id++;
        light_object_init(&dev->header, &ltype_audio_device);
        dev->device_id = device_id;
        dev->driver_ctx = driver_ctx;
        // light_object_alloc() doesn't zero, same as every other device in this tree. an
        // uninitialised owned_buffer would be freed on the first poll
        dev->volume = LIGHT_AUDIO_VOLUME_MAX;
        dev->owned_buffer = NULL;
        dev->playing = false;
        dev->play_start_ms = 0;
        dev->toning = false;
        dev->tone_end_ms = 0;

        light_object_add_va(&dev->header, &device_root.header, format, args);
        return dev;
}
void light_audio_command_init(struct audio_device *dev)
{
        light_debug("device: %s", dev->header.id);
        dev->driver_ctx->driver->init_device(dev);
}

void light_audio_set_volume(struct audio_device *dev, uint16_t volume)
{
        dev->volume = volume > LIGHT_AUDIO_VOLUME_MAX ? LIGHT_AUDIO_VOLUME_MAX : volume;
}

uint8_t light_audio_pcm_to_duty(int32_t sample, uint8_t encoding, uint16_t volume)
{
        if(volume > LIGHT_AUDIO_VOLUME_MAX)
                volume = LIGHT_AUDIO_VOLUME_MAX;

        // everything is brought to a signed value centred on zero first, so there is one
        // attenuation and one re-centring rather than a separate path per encoding
        int32_t centred;
        if(encoding == LIGHT_AUDIO_PCM_U8) {
                // U8 PCM is centred on 128, so the shift is the encoding's own bias
                centred = (sample & 0xFF) - 128;                 // -128..127
        } else {
                // S16 already straddles zero; >>8 lands it in the same -128..127 window.
                // an arithmetic shift, NOT a divide by 256: a divide truncates towards zero,
                // which maps two adjacent input codes onto the same output either side of
                // silence and puts a flat step in the middle of every waveform
                if(sample < -32768) sample = -32768;
                if(sample > 32767)  sample = 32767;
                centred = sample >> 8;                           // -128..127
        }

        // attenuate about zero -- which after re-centring below is silence, not the bottom of
        // the range. scaling the UNSIGNED value instead would slide the DC level down with
        // the volume, and a piezo turns a DC slide into a click
        centred = (centred * (int32_t)volume) / LIGHT_AUDIO_VOLUME_MAX;

        int32_t duty = centred + LIGHT_AUDIO_DUTY_SILENCE;
        // the arithmetic above cannot leave this range -- centred is -128..127 and silence is
        // 128 -- but the clamp costs nothing and means a future encoding cannot silently wrap
        // the duty register past its wrap value
        if(duty < 0) duty = 0;
        if(duty > LIGHT_AUDIO_DUTY_MAX) duty = LIGHT_AUDIO_DUTY_MAX;
        return (uint8_t)duty;
}

// true when the source is already exactly what the driver wants, so it can be played where it
// lies. only U8 at full volume qualifies: the conversion is the identity there, which is
// worth checking against rather than asserting -- see the harness
static bool _needs_no_conversion(const struct audio_format *format, uint16_t volume)
{
        return format->encoding == LIGHT_AUDIO_PCM_U8 && volume == LIGHT_AUDIO_VOLUME_MAX;
}

static void _release_buffer(struct audio_device *dev)
{
        if(!dev->owned_buffer)
                return;
        light_free(dev->owned_buffer);
        dev->owned_buffer = NULL;
}

bool light_audio_play_pcm(struct audio_device *dev, const void *samples,
                        uint32_t frame_count, struct audio_format format)
{
        if(!samples || !frame_count || !format.sample_rate)
                return false;
        if(format.channels > 1) {
                light_warn("light_audio: %d channels requested, only mono is supported",
                                format.channels);
                return false;
        }
        if(format.encoding != LIGHT_AUDIO_PCM_U8 && format.encoding != LIGHT_AUDIO_PCM_S16) {
                light_warn("light_audio: unsupported encoding %d", format.encoding);
                return false;
        }
        if(dev->driver_ctx->driver->busy(dev))
                return false;

        // whatever the previous sound left behind, before anything new is allocated
        _release_buffer(dev);

        const uint8_t *duty;
        if(_needs_no_conversion(&format, dev->volume)) {
                duty = (const uint8_t *)samples;
        } else {
                uint8_t *converted = light_alloc(frame_count);
                if(!converted) {
                        light_error("light_audio: could not allocate %d bytes for conversion",
                                        (int)frame_count);
                        return false;
                }
                if(format.encoding == LIGHT_AUDIO_PCM_S16) {
                        const int16_t *src = (const int16_t *)samples;
                        for(uint32_t i = 0; i < frame_count; i++)
                                converted[i] = light_audio_pcm_to_duty(src[i],
                                                LIGHT_AUDIO_PCM_S16, dev->volume);
                } else {
                        const uint8_t *src = (const uint8_t *)samples;
                        for(uint32_t i = 0; i < frame_count; i++)
                                converted[i] = light_audio_pcm_to_duty(src[i],
                                                LIGHT_AUDIO_PCM_U8, dev->volume);
                }
                dev->owned_buffer = converted;
                duty = converted;
        }

        // a tone and a sample stream are different configurations of the same slice, so
        // starting one has to end the other
        dev->toning = false;
        if(!dev->driver_ctx->driver->submit(dev, duty, frame_count, format.sample_rate)) {
                _release_buffer(dev);
                return false;
        }
        dev->playing = true;
        dev->play_start_ms = light_platform_get_time_since_init();
        return true;
}

void light_audio_tone(struct audio_device *dev, uint32_t hz, uint32_t duration_ms)
{
        // the sample path owns the slice until it is stopped, and its buffer has to go back
        // before the slice is reconfigured underneath the DMA
        dev->driver_ctx->driver->stop(dev);
        _release_buffer(dev);
        dev->playing = false;

        dev->driver_ctx->driver->tone(dev, hz);
        if(!hz) {
                dev->toning = false;
                return;
        }
        dev->toning = true;
        // 0 means "until stopped" -- tone_end_ms is only consulted while a duration is set,
        // so there is no sentinel to get wrong
        dev->tone_end_ms = duration_ms
                        ? light_platform_get_time_since_init() + duration_ms : 0;
}

void light_audio_stop(struct audio_device *dev)
{
        dev->driver_ctx->driver->stop(dev);
        _release_buffer(dev);
        dev->playing = false;
        dev->toning = false;
}

bool light_audio_is_playing(const struct audio_device *dev)
{
        return dev->playing || dev->toning;
}

void light_audio_poll_devices(void)
{
        uint32_t now = light_platform_get_time_since_init();
        for(uint16_t i = 0; i < next_device_id; i++) {
                struct audio_device *dev = device_root.device[i];
                if(!dev)
                        continue;

                // a tone ends on the clock; samples end when the transfer runs out. the two
                // are mutually exclusive, but polled independently so neither can strand the
                // other's cleanup
                if(dev->toning && dev->tone_end_ms
                                && (int32_t)(now - dev->tone_end_ms) >= 0) {
                        dev->driver_ctx->driver->tone(dev, 0);
                        dev->toning = false;
                }
                if(dev->playing && !dev->driver_ctx->driver->busy(dev)) {
                        // the elapsed time is the diagnostic worth having: a buffer that
                        // played for about as long as its sample count implies really did
                        // stream, and one that finishes in a few milliseconds never started
                        light_debug("playback finished after %d ms",
                                        (int)(now - dev->play_start_ms));
                        dev->driver_ctx->driver->stop(dev);
                        // only now is the conversion buffer safe to release: until busy()
                        // goes false the DMA is still reading out of it
                        _release_buffer(dev);
                        dev->playing = false;
                }
        }
}
