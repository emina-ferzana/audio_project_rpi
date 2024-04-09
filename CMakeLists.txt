cmake_minimum_required(VERSION 3.10)
project(MyAudioProject)

# Set the C standard to C99
set(CMAKE_C_STANDARD 99)


# Specify the source files for the two programs
add_executable(pa_devs src/pa_devs.c)
add_executable(record_example src/record_example.c)
add_executable(test_fftw src/test_fftw.c)
add_executable(continuous_capture src/continuous_capture.c)

# Link the executables to the PortAudio library
target_link_libraries(pa_devs portaudio  asound m pthread rt)
target_link_libraries(record_example portaudio  asound m pthread rt)
target_link_libraries(test_fftw sndfile m fftw3)

# Add the directory where pa_ringbuffer.h is located to your include path
target_include_directories(continuous_capture PRIVATE "/home/pi/project/lib/include" "/home/pi/project/lib/portaudio/src/common")
target_link_libraries(continuous_capture portaudio asound pthread m)

# Set the output directory for the binaries
set_target_properties(pa_devs
                    record_example
                    test_fftw 
                    continuous_capture
                    PROPERTIES
                    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
