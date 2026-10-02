cmake_minimum_required(VERSION 3.20)
project(xsr_consumer LANGUAGES CXX)

# The consumer only ever sees the installed package: it is configured with a
# prefix path and never with the producer's build tree.
find_package(xsr 1.0 CONFIG REQUIRED)

add_executable(xsr_consumer main.cpp)
target_link_libraries(xsr_consumer PRIVATE Summon::xsr)
target_compile_features(xsr_consumer PRIVATE cxx_std_20)
