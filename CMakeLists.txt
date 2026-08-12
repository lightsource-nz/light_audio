
add_library(light_audio INTERFACE)

target_include_directories(
        light_audio INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_sources(
        light_audio INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/src/module.c
        ${CMAKE_CURRENT_LIST_DIR}/src/audio.c
        ${CMAKE_CURRENT_LIST_DIR}/src/audio_pwm.c
)

# no light_ioport, on the same terms as light_backlight and light_button: a buzzer is driven
# as a PWM output on a pin, not over a transport bus, so there is no io_context to open
target_link_libraries(
        light_audio INTERFACE
        light_core
)

if(LIGHT_SYSTEM STREQUAL PICO_SDK AND LIGHT_PLATFORM STREQUAL TARGET)
        target_link_libraries(
                light_audio INTERFACE
                hardware_clocks
                hardware_dma
                hardware_gpio
                hardware_pwm
        )
endif()

# tests are host-only: the binary has to run where it is built. BUILD_TESTING is defined by
# include(CTest) in whatever project pulls this module in, so a consumer that never enabled
# testing simply does not get them
if(BUILD_TESTING AND LIGHT_PLATFORM STREQUAL HOST)
        add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/test light_audio_test)
endif()
