#include <light_audio.h>

#include "light_audio_internal.h"

static void _module_event(const struct light_module *module, uint8_t event, void *arg);
Light_Module_Define(light_audio, _module_event,
                                &light_core);

static uint8_t _module_task(struct light_application *app);
static void _module_event(const struct light_module *module, uint8_t event, void *arg)
{
        switch(event) {
                case LF_EVENT_MODULE_LOAD:
                light_audio_init();
                // needed for the same reason light_backlight's is: a tone has to stop when
                // its duration runs out, and a conversion buffer has to be released when the
                // transfer finishes, without the application being obliged to notice either
                light_module_register_periodic_task(&light_audio, "light_audio_task", _module_task);
                break;
                // TODO implement unregister for event hooks
                case LF_EVENT_MODULE_UNLOAD:
                break;
        }
}
static uint8_t _module_task(struct light_application *app)
{
        light_audio_poll_devices();
        return LF_STATUS_RUN;
}
