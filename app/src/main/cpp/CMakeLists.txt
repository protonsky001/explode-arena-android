cmake_minimum_required(VERSION 3.10)
project(explodearena C)

# Native core: ARM interpreter + Symbian OS services (see ea_host.c) and the JNI bridge.
add_library(explodearena SHARED ea_host.c jni_bridge.c)
target_compile_options(explodearena PRIVATE -O2 -fno-strict-aliasing -Wno-unused-function)
target_link_libraries(explodearena log)
