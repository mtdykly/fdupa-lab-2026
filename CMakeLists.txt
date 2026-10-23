cmake_minimum_required(VERSION 3.16)
project(fdulang VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(fdulang_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(fdulang_unit_test target source name label)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE fdulang_core)
    fdulang_warnings(${target})
    add_test(NAME ${name} COMMAND ${target} ${ARGN})
    set_tests_properties(${name} PROPERTIES LABELS "${label}" TIMEOUT 60)
endfunction()

add_library(fdulang_core
    src/frontend.cpp
    src/ir.cpp
    src/runtime.cpp
    src/analysis.cpp
    src/interprocedural.cpp
    src/domains/interval_support.cpp
    src/domains/octagon_support.cpp
    src/labs/lab1_interval.cpp
    src/labs/lab2_octagon.cpp
    src/labs/lab3_context.cpp
)
target_include_directories(fdulang_core PUBLIC include)
fdulang_warnings(fdulang_core)

add_executable(fdulang src/main.cpp)
target_link_libraries(fdulang PRIVATE fdulang_core)
fdulang_warnings(fdulang)

find_package(Python3 COMPONENTS Interpreter QUIET)
option(FDULANG_BUILD_LAB_TESTS "Build analysis contract tests (separate from language tests)" ON)
if(FDULANG_BUILD_LAB_TESTS)
    enable_testing()
    fdulang_unit_test(fdulang_analysis_tests tests/labs/unit.cpp labs.unit "labs;lab1")
    fdulang_unit_test(fdulang_octagon_tests tests/labs/octagon_unit.cpp labs.octagon_unit "labs;lab2")
    fdulang_unit_test(fdulang_interprocedural_tests tests/labs/interprocedural_unit.cpp
        labs.inter_context_unit "labs;lab3-context" hooks/context)
    foreach(DOMAIN interval octagon)
        add_test(NAME labs.inter_${DOMAIN}_unit COMMAND fdulang_interprocedural_tests ${DOMAIN})
        set_tests_properties(labs.inter_${DOMAIN}_unit PROPERTIES LABELS "labs;lab3-${DOMAIN}" TIMEOUT 60)
    endforeach()
    add_test(NAME labs.inter_bottom_embed_unit COMMAND fdulang_interprocedural_tests bridge/embed_preserves_bottom)
    add_test(NAME labs.inter_bottom_return_unit COMMAND fdulang_interprocedural_tests bridge/bottom_return_has_no_continuation)
    add_test(NAME labs.inter_relations_unit COMMAND fdulang_interprocedural_tests relations/)
    set_tests_properties(labs.inter_bottom_embed_unit labs.inter_bottom_return_unit labs.inter_relations_unit
        PROPERTIES LABELS "labs;lab3-comparison" TIMEOUT 60)

    if(Python3_Interpreter_FOUND)
        foreach(DOMAIN interval octagon)
            if(DOMAIN STREQUAL "interval")
                set(CLI_NAME labs.cli)
                set(LAB_LABEL lab1)
            else()
                set(CLI_NAME labs.octagon_cli)
                set(LAB_LABEL lab2)
            endif()
            add_test(NAME ${CLI_NAME}
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/labs/cli_tests.py"
                    --binary "$<TARGET_FILE:fdulang>" --domain ${DOMAIN} --implementation student)
            set_tests_properties(${CLI_NAME} PROPERTIES LABELS "labs;${LAB_LABEL}" TIMEOUT 60)
            add_test(NAME labs.inter_${DOMAIN}_cli
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/labs/interprocedural_cli_tests.py"
                    --binary "$<TARGET_FILE:fdulang>" --domain ${DOMAIN} --implementation student)
            set_tests_properties(labs.inter_${DOMAIN}_cli PROPERTIES LABELS "labs;lab3-${DOMAIN}" TIMEOUT 60)
        endforeach()
        add_test(NAME labs.evaluator
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/labs/evaluator_tests.py")
        set_tests_properties(labs.evaluator PROPERTIES LABELS "labs;labs-common" TIMEOUT 60)
    endif()
endif()

option(FDULANG_BUILD_LANGUAGE_TESTS "Build language infrastructure tests (not lab tests)" ON)
if(FDULANG_BUILD_LANGUAGE_TESTS)
    enable_testing()
    fdulang_unit_test(fdulang_language_tests tests/language/main.cpp language.unit language)
    if(Python3_Interpreter_FOUND)
        add_test(NAME language.cli
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/language/cli_tests.py"
                --binary "$<TARGET_FILE:fdulang>")
        set_tests_properties(language.cli PROPERTIES LABELS "language" TIMEOUT 60)
    endif()
endif()
