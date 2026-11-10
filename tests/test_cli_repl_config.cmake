function(run_repl_session input timeout result)
    execute_process(
        COMMAND "${CLI}" --interactive --lang en
        INPUT_FILE "${input}"
        OUTPUT_VARIABLE output
        ERROR_VARIABLE diagnostic
        RESULT_VARIABLE status
        TIMEOUT ${timeout})
    file(REMOVE "${input}")
    if(NOT status STREQUAL "0")
        message(FATAL_ERROR "Interactive session exited with ${status}: ${diagnostic}")
    endif()
    string(REPLACE "\r\n" "\n" output "${output}")
    set(${result} "${output}" PARENT_SCOPE)
endfunction()

# Feed a complete interactive session to the built CLI, including aliases.
string(RANDOM LENGTH 20 suffix)
set(input "${CMAKE_CURRENT_BINARY_DIR}/repl-config-${suffix}.txt")
file(WRITE "${input}" [=[
config
set fb_algebraic 47
config
reset
set lp_bound 1009
config
reset
method trial
config
method auto
config
reset
config
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
run_repl_session("${input}" 12 output)
foreach(key IN ITEMS degree rational_bound algebraic_bound large_prime_bound)
    string(FIND "${output}" "Invalid value: Config: ${key} must be positive" index)
    if(index EQUAL -1)
        message(FATAL_ERROR "Missing rejection of zero ${key}: ${output}")
    endif()
endforeach()
foreach(setting IN ITEMS "degree = 4" "rational_bound = 31" "algebraic_bound = 43"
                         "large_prime_bound = 65539" "algebraic_bound = 47"
                         "large_prime_bound = 1009" "method = trial" "method = auto")
    string(FIND "${output}" "\n${setting}\n" index)
    if(index EQUAL -1)
        message(FATAL_ERROR "Lost configured ${setting}: ${output}")
    endif()
endforeach()
if(NOT output MATCHES "\"success\"[ \t]*:[ \t]*true")
    message(FATAL_ERROR "Factorization after a rejected setting failed: ${output}")
endif()

# Only the initial and reset configurations have no factorization overrides.
string(REGEX MATCHALL "no overrides" summaries "${output}")
string(REGEX MATCHALL "# GNFS Configuration\nverbose = true\n#[^\n]*no overrides"
       default_summaries "${output}")
list(LENGTH summaries summary_count)
list(LENGTH default_summaries default_count)
if(NOT summary_count EQUAL 2 OR NOT default_count EQUAL 2)
    message(FATAL_ERROR "Incorrect auto-detection summary in interactive config: ${output}")
endif()

# A redirected CRLF session starts with an empty line and a whitespace-only line.
file(CONFIGURE OUTPUT "${input}" CONTENT "\n \t\nnot-a-number\nquiet\njson\n360\nquit\n"
     NEWLINE_STYLE CRLF)
file(READ "${input}" prefix LIMIT 6 HEX)
run_repl_session("${input}" 6 output)

if(NOT prefix STREQUAL "0d0a20090d0a")
    message(FATAL_ERROR "Unexpected CRLF fixture bytes: ${prefix}")
endif()
string(REGEX MATCHALL "Error:" errors "${output}")
list(LENGTH errors error_count)
if(NOT error_count EQUAL 1 OR
   NOT output MATCHES "Error: not-a-number" OR NOT output MATCHES "\"success\"[ \t]*:[ \t]*true")
    message(FATAL_ERROR "CRLF session should reject only the nonempty invalid input: ${output}")
endif()
