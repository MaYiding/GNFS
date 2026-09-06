cmake_minimum_required(VERSION 3.20)

foreach(required IN ITEMS SUPERVISOR FAKE_CHILD TEST_DIRECTORY)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "missing -D${required}=...")
    endif()
endforeach()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef contract_nonce)
set(contract_directory "${TEST_DIRECTORY}/${contract_nonce}")
file(MAKE_DIRECTORY "${contract_directory}")
set(stdout_file "${contract_directory}/stdout.bin")
set(stderr_file "${contract_directory}/stderr.bin")
set(ready_file "${contract_directory}/tree.ready")
set(descendant_ready_file "${ready_file}.descendant")
set(survived_file "${contract_directory}/tree.survived")
file(REMOVE "${stdout_file}" "${stderr_file}" "${ready_file}"
    "${descendant_ready_file}" "${survived_file}")

execute_process(
    COMMAND "${SUPERVISOR}"
        --timeout-ms 3000
        --output-limit-bytes 4096
        --stdout-file "${contract_directory}/missing/stdout.bin"
        --stderr-file "${stderr_file}"
        -- "${FAKE_CHILD}" --hang
    RESULT_VARIABLE initialize_result
    OUTPUT_VARIABLE initialize_stdout
    ERROR_VARIABLE initialize_stderr)
if(NOT "${initialize_result}" STREQUAL "125" OR
   NOT "${initialize_stdout}" STREQUAL "" OR
   NOT initialize_stderr MATCHES "could not initialize output files")
    message(FATAL_ERROR
        "output initialization failure mapping changed: result=${initialize_result} stdout=${initialize_stdout} stderr=${initialize_stderr}")
endif()

execute_process(
    COMMAND "${SUPERVISOR}"
    RESULT_VARIABLE usage_result
    OUTPUT_VARIABLE usage_stdout
    ERROR_VARIABLE usage_stderr)
if(NOT "${usage_result}" STREQUAL "64" OR
   NOT "${usage_stdout}" STREQUAL "" OR NOT usage_stderr MATCHES "usage:")
    message(FATAL_ERROR
        "usage failure mapping changed: result=${usage_result} stdout=${usage_stdout} stderr=${usage_stderr}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "BCP_TEST_ENV=supervisor-ambient"
        "BCP_PARENT_ONLY=visible"
        "${SUPERVISOR}"
        --timeout-ms 3000
        --output-limit-bytes 4096
        -- "${FAKE_CHILD}" --echo "参数-π"
    RESULT_VARIABLE ambient_result
    OUTPUT_VARIABLE ambient_stdout
    ERROR_VARIABLE ambient_stderr)
if(NOT "${ambient_result}" STREQUAL "0")
    message(FATAL_ERROR
        "ambient run failed: result=${ambient_result} stderr=${ambient_stderr}")
endif()
set(expected_ambient_stdout
    "argument_count=1\nargument_0=参数-π\nenvironment=supervisor-ambient\nparent_only=<present>\n")
if(NOT "${ambient_stdout}" STREQUAL "${expected_ambient_stdout}" OR
   NOT "${ambient_stderr}" STREQUAL "")
    message(FATAL_ERROR
        "ambient argv/environment bytes changed: stdout=${ambient_stdout} stderr=${ambient_stderr}")
endif()

execute_process(
    COMMAND "${SUPERVISOR}"
        --timeout-ms 3000
        --output-limit-bytes 4096
        --stdout-file "${stdout_file}"
        --stderr-file "${stderr_file}"
        -- "${FAKE_CHILD}" --nonzero
    RESULT_VARIABLE nonzero_result
    OUTPUT_VARIABLE nonzero_control_stdout
    ERROR_VARIABLE nonzero_control_stderr)
if(NOT "${nonzero_result}" STREQUAL "23")
    message(FATAL_ERROR
        "nonzero run changed exit code: result=${nonzero_result} stderr=${nonzero_control_stderr}")
endif()

if(GNFS_TEST_UNIX)
    execute_process(
        COMMAND "${SUPERVISOR}"
            --timeout-ms 3000
            --output-limit-bytes 4096
            -- "${FAKE_CHILD}" --signal
        RESULT_VARIABLE signaled_result
        OUTPUT_VARIABLE signaled_stdout
        ERROR_VARIABLE signaled_stderr)
    if(NOT "${signaled_result}" STREQUAL "143" OR
       NOT "${signaled_stdout}" STREQUAL "" OR NOT "${signaled_stderr}" STREQUAL "")
        message(FATAL_ERROR
            "signaled-child mapping changed: result=${signaled_result} stdout=${signaled_stdout} stderr=${signaled_stderr}")
    endif()
endif()
file(READ "${stdout_file}" file_stdout)
file(READ "${stderr_file}" file_stderr)
if(NOT "${file_stdout}" STREQUAL "stdout-before-nonzero\n" OR
   NOT "${file_stderr}" STREQUAL "stderr-before-nonzero\n" OR
   NOT "${nonzero_control_stdout}" STREQUAL "" OR
   NOT "${nonzero_control_stderr}" STREQUAL "")
    message(FATAL_ERROR
        "dual-file bytes changed: stdout=${file_stdout} stderr=${file_stderr}")
endif()

execute_process(
    COMMAND "${SUPERVISOR}"
        --timeout-ms 3000
        --output-limit-bytes 4096
        --combined-output
        -- "${FAKE_CHILD}" --interleaved 12 3
    RESULT_VARIABLE combined_result
    OUTPUT_VARIABLE combined_stdout
    ERROR_VARIABLE combined_stderr)
if(NOT "${combined_result}" STREQUAL "0" OR
   NOT "${combined_stdout}" STREQUAL "OOOEEEOOOEEEOOOEEEOOOEEE" OR
   NOT "${combined_stderr}" STREQUAL "")
    message(FATAL_ERROR
        "combined 2>&1 order changed: result=${combined_result} stdout=${combined_stdout} stderr=${combined_stderr}")
endif()

execute_process(
    COMMAND "${SUPERVISOR}"
        --timeout-ms 3000
        --output-limit-bytes 4
        -- "${FAKE_CHILD}" --write-sizes 8 0
    RESULT_VARIABLE overflow_result
    OUTPUT_VARIABLE overflow_stdout
    ERROR_VARIABLE overflow_stderr)
if(NOT "${overflow_result}" STREQUAL "125" OR
   NOT "${overflow_stdout}" STREQUAL "OOOO" OR
   NOT overflow_stderr MATCHES "transport=overflow")
    message(FATAL_ERROR
        "overflow failure mapping changed: result=${overflow_result} stdout=${overflow_stdout} stderr=${overflow_stderr}")
endif()

execute_process(
    COMMAND "${SUPERVISOR}"
        --timeout-ms 2000
        --output-limit-bytes 4096
        -- "${FAKE_CHILD}" --timeout-tree "${survived_file}" 2500 "${ready_file}"
    RESULT_VARIABLE timeout_result
    OUTPUT_VARIABLE timeout_stdout
    ERROR_VARIABLE timeout_stderr)
if(NOT "${timeout_result}" STREQUAL "124")
    message(FATAL_ERROR
        "tree timeout did not map to 124: result=${timeout_result} stderr=${timeout_stderr}")
endif()
file(READ "${ready_file}" ready_contents)
file(READ "${descendant_ready_file}" descendant_ready_contents)
if(NOT "${ready_contents}" STREQUAL "tree-ready\n" OR
   NOT "${descendant_ready_contents}" STREQUAL "descendant-ready\n")
    message(FATAL_ERROR "tree timeout did not establish the descendant-ready precondition")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 3)
if(EXISTS "${survived_file}")
    message(FATAL_ERROR "a timed-out descendant survived long enough to publish its marker")
endif()
if(NOT "${timeout_stdout}" STREQUAL "descendant-ready\n" OR
   NOT "${timeout_stderr}" STREQUAL "")
    message(FATAL_ERROR
        "tree timeout output changed: stdout=${timeout_stdout} stderr=${timeout_stderr}")
endif()

file(REMOVE_RECURSE "${contract_directory}")
