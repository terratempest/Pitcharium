cmake_minimum_required(VERSION 3.22.1)

project("spectrogram-jni")

# Add the shared library
add_library(
    spectrogram-jni
    SHARED
    spectrogram-jni.cpp
    pro-estimator.cpp
)

target_compile_options(spectrogram-jni PRIVATE
    $<$<CONFIG:Debug>:-O2;-g>
)

# Link log library for __android_log_print
find_library(
    log-lib
    log
)

target_link_libraries(
    spectrogram-jni
    ${log-lib}
)
