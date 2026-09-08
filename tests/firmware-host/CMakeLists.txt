cmake_minimum_required(VERSION 3.16)

project(factory_timer_host_tests LANGUAGES CXX)

enable_testing()

foreach(debug_logs IN ITEMS 0 1)
    set(test_target factory_timer_core_debug_${debug_logs})
    add_executable(
        ${test_target}
        command_processing_test.cpp
        ../../main/command_processor.cpp
        ../../main/countdown_timer.cpp
        ../../main/protocol_codec.cpp
        ../../main/network_policy.cpp
    )
    target_compile_features(${test_target} PRIVATE cxx_std_17)
    target_compile_definitions(${test_target} PRIVATE CONFIG_FACTORY_DEBUG_LOGS=${debug_logs})
    target_include_directories(${test_target} PRIVATE stubs ../../main)
    add_test(NAME ${test_target} COMMAND ${test_target})
endforeach()
