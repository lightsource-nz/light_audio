#ifndef _LIGHT_AUDIO_INTERNAL_H
#define _LIGHT_AUDIO_INTERNAL_H

// called once per scheduler tick by light_audio's own periodic task (module.c): ends a tone
// whose duration has run out, and releases the conversion buffer once the driver reports the
// samples have finished. not meant to be called by application code, hence kept out of the
// public header
extern void light_audio_poll_devices(void);

#endif
