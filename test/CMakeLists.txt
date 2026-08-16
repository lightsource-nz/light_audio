
# HOST ONLY -- see the guard in the parent CMakeLists. the conversion under test is pure
# arithmetic, so it needs no hardware, but the binary still has to run on the build machine.
add_executable(test_audio_convert ${CMAKE_CURRENT_LIST_DIR}/test_audio_convert.c)

# light_audio is an INTERFACE library carrying its sources and its light_core dependency, so
# linking it gives the test the real headers rather than a stubbed stand-in. that matters
# here: the conversion is exercised exactly as the firmware calls it
target_link_libraries(test_audio_convert PRIVATE light_audio)

# one CTest case per test function. KEEP IN SYNC with test_cases[] in test_audio_convert.c;
# `test_audio_convert --list` prints what the binary has, and a name that does not exist
# exits non-zero rather than passing quietly
set(LIGHT_AUDIO_TEST_CASES
        anchors
        u8_passthrough_identity
        monotonic
        volume_to_silence
        half_volume
        clamping
        symmetry
        uniform_code_spans
        over_range_volume
        extreme_input
)
foreach(_case ${LIGHT_AUDIO_TEST_CASES})
        add_test(NAME light_audio.${_case} COMMAND test_audio_convert ${_case})
endforeach()

#   the playback lifecycle, against a fake driver. A separate binary rather than more cases in
# the one above because it needs a driver registered and light_audio_init() called, which the
# arithmetic tests deliberately do without -- keeping them in one binary would mean every pure
# function test running against a half-started module
add_executable(test_audio_playback ${CMAKE_CURRENT_LIST_DIR}/test_audio_playback.c)
target_link_libraries(test_audio_playback PRIVATE light_audio)

# KEEP IN SYNC with test_cases[] in test_audio_playback.c -- `test_audio_playback --list`
# prints what the binary actually has
set(LIGHT_AUDIO_PLAYBACK_TEST_CASES
        play_pcm_rejects_bad_arguments
        play_pcm_refuses_while_busy
        u8_at_full_volume_played_in_place
        conversion_allocates_and_matches
        failed_submit_releases_buffer
        poll_releases_buffer_only_when_idle
        tone_and_samples_exclusive
        poll_ends_tone_on_the_clock
        tone_without_duration
        set_volume_clamps
)
foreach(_case ${LIGHT_AUDIO_PLAYBACK_TEST_CASES})
        add_test(NAME light_audio.${_case} COMMAND test_audio_playback ${_case})
endforeach()

#   the PWM driver, against a fake peripheral. A third binary rather than more cases in either of
# the two above, because this one replaces part of the platform under the whole process: the
# --wrap flags below apply to the entire link, so a test that wanted the REAL host PWM could not
# share a binary with it.
add_executable(test_audio_pwm ${CMAKE_CURRENT_LIST_DIR}/test_audio_pwm.c)
target_link_libraries(test_audio_pwm PRIVATE light_audio)

#   -Wl,--wrap redirects each call to __wrap_<symbol>, which the test defines. This is needed
# rather than simply defining the functions again because light_core_arch_host_os contributes
# light_platform_pwm.c to every target as an INTERFACE source -- a second definition would be a
# duplicate symbol at link, not an override.
#
#   EVERY symbol audio_pwm.c calls has to be listed. A missing one links silently against the
# host stub, which returns NULL from open() and does nothing else, so the test would exercise
# the absent-hardware path while appearing to test the real one -- passing for the wrong reason.
foreach(_sym
        light_platform_pwm_open
        light_platform_pwm_close
        light_platform_pwm_configure
        light_platform_pwm_set_frequency
        light_platform_pwm_set_duty
        light_platform_pwm_set_enabled
        light_platform_pwm_release_pin
        light_platform_pwm_get_group)
        target_link_options(test_audio_pwm PRIVATE -Wl,--wrap=${_sym})
endforeach()

# KEEP IN SYNC with test_cases[] in test_audio_pwm.c
set(LIGHT_AUDIO_PWM_TEST_CASES
        open_parks_the_output_silent
        tone_sets_frequency_and_half_duty
        tone_zero_drives_the_pin_low
        destroy_releases_low_then_closes
        absent_pwm_is_tolerated
        fallbacks_without_pwm_streaming
)
foreach(_case ${LIGHT_AUDIO_PWM_TEST_CASES})
        add_test(NAME light_audio.${_case} COMMAND test_audio_pwm ${_case})
endforeach()
