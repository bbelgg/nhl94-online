# End-to-end synthetic test of the genesis profile's JSR/BSR return-slot
# handling: a generator links the real function finder/code generator, emits C
# for a small hand-assembled 68000 program, and a runner compiles that C
# against a stub runtime and asserts PC flow, A7 and registers.
# No ROM or consumer code is needed; game_config.h/rom_parser.h/
# genesis_runtime.h here are test-owned stand-ins.

add_executable(m68k_flow_gen
    flow_gen.c game_config.c
    ../../common/m68k_decoder.c ../../common/m68k_validator.c
    ../../common/m68k_timing.c ../../common/annotations.c
    ../../profiles/genesis/return_capture.c
    ../../profiles/genesis/function_finder.c
    ../../profiles/genesis/code_generator.c
    ../../profiles/genesis/codegen_diag.c)
target_include_directories(m68k_flow_gen PRIVATE
    . ../../common ../../profiles/genesis)
target_compile_features(m68k_flow_gen PRIVATE c_std_11)

set(_flow_variants plain declared oracle)
set(_flow_id_plain 0)
set(_flow_id_declared 1)
set(_flow_id_oracle 2)
foreach(_v IN LISTS _flow_variants)
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/gen_${_v}")
    set(_srcs "${_out}/flow_dispatch.c")
    foreach(_i RANGE 0 31)
        string(LENGTH "${_i}" _len)
        if(_len EQUAL 1)
            set(_n "0${_i}")
        else()
            set(_n "${_i}")
        endif()
        list(APPEND _srcs "${_out}/flow_part${_n}.c")
    endforeach()
    add_custom_command(
        OUTPUT ${_srcs} "${_out}/flow_decls.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_out}"
        COMMAND m68k_flow_gen ${_v} "${_out}"
        DEPENDS m68k_flow_gen
        COMMENT "Generating synthetic flow program (${_v})"
        VERBATIM)
    add_executable(m68k_flow_${_v}_test flow_run.c ${_srcs})
    target_include_directories(m68k_flow_${_v}_test PRIVATE . "${_out}")
    target_compile_definitions(m68k_flow_${_v}_test PRIVATE FLOW_VARIANT=${_flow_id_${_v}})
    target_compile_features(m68k_flow_${_v}_test PRIVATE c_std_11)
    add_test(NAME m68k_flow_${_v} COMMAND m68k_flow_${_v}_test)
endforeach()

# dynamic_return_slots off: the generator must emit upstream's static-count
# call/return form (checked on the emitted text; not run).
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gen_upstream")
add_test(NAME m68k_flow_upstream_form
         COMMAND m68k_flow_gen upstream "${CMAKE_CURRENT_BINARY_DIR}/gen_upstream")
