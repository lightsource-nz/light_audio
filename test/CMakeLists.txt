
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
