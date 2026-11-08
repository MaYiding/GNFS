# Feed a complete interactive session to the built CLI, including aliases.
string(RANDOM LENGTH 20 suffix)
set(input "${CMAKE_CURRENT_BINARY_DIR}/repl-config-${suffix}.txt")
file(WRITE "${input}" [=[
quiet
set degree 4
set rational_bound 31
set algebraic_bound 43
set large_prime_bound 65539
set degree 0
set fb_rational 0
set fb_algebraic 0
set lp_bound 0
config
json
360
quit
]=])
execute_process(
    COMMAND "${CLI}" --interactive --lang en
    INPUT_FILE "${input}"
    OUTPUT_VARIABLE output
    ERROR_VARIABLE diagnostic
    RESULT_VARIABLE status
    TIMEOUT 12)
file(REMOVE "${input}")

if(NOT status STREQUAL "0")
    message(FATAL_ERROR "Interactive session exited with ${status}: ${diagnostic}")
endif()
string(REPLACE "\r\n" "\n" output "${output}")
foreach(key IN ITEMS degree rational_bound algebraic_bound large_prime_bound)
    string(FIND "${output}" "Invalid value: Config: ${key} must be positive" index)
    if(index EQUAL -1)
        message(FATAL_ERROR "Missing rejection of zero ${key}: ${output}")
    endif()
endforeach()
foreach(setting IN ITEMS "degree = 4" "rational_bound = 31" "algebraic_bound = 43"
                         "large_prime_bound = 65539")
    string(FIND "${output}" "\n${setting}\n" index)
    if(index EQUAL -1)
        message(FATAL_ERROR "Lost configured ${setting}: ${output}")
    endif()
endforeach()
if(NOT output MATCHES "\"success\"[ \t]*:[ \t]*true")
    message(FATAL_ERROR "Factorization after a rejected setting failed: ${output}")
endif()
