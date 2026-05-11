# CMake script-mode (`cmake -P`) used as a post-build step on libcvm.a
# under the Cortex-M sanity toolchain. Runs `nm --undefined-only` on
# the static archive and confirms every undefined symbol either:
#
#   1. Matches an entry in the allowlist file (`ALLOWLIST`), or
#   2. Matches a `<prefix>*` wildcard from that file.
#
# Anything else fails the build with a descriptive message. The
# canonical regression this guards against is malloc/free leaking back
# in when CVM_NO_STDLIB_FALLBACK should have removed them.
#
# Required cache-style args (passed via `cmake -DKEY=VAL -P`):
#   LIBRARY    — path to libcvm.a
#   NM         — path to the arm-none-eabi-nm binary
#   ALLOWLIST  — path to cmake/cortex_m_allowed_symbols.txt

if(NOT LIBRARY OR NOT NM OR NOT ALLOWLIST)
    message(FATAL_ERROR
        "check_cvm_freestanding.cmake: missing required arg "
        "(LIBRARY=${LIBRARY} NM=${NM} ALLOWLIST=${ALLOWLIST})")
endif()

# Parse the allowlist into two lists: exact matches and prefix matches.
file(STRINGS "${ALLOWLIST}" _allow_lines)
set(_allow_exact "")
set(_allow_prefix "")
foreach(_line ${_allow_lines})
    string(STRIP "${_line}" _line)
    if(_line STREQUAL "" OR _line MATCHES "^#")
        continue()
    endif()
    string(LENGTH "${_line}" _len)
    math(EXPR _last "${_len} - 1")
    string(SUBSTRING "${_line}" ${_last} 1 _tail)
    if(_tail STREQUAL "*")
        string(SUBSTRING "${_line}" 0 ${_last} _prefix)
        list(APPEND _allow_prefix "${_prefix}")
    else()
        list(APPEND _allow_exact "${_line}")
    endif()
endforeach()

# Run nm against the archive. `--undefined-only` skips defined symbols;
# the format we parse is `<spaces>U <name>` (one per line). Multiple
# object files in the archive print headers we ignore.
execute_process(
    COMMAND ${NM} --undefined-only ${LIBRARY}
    OUTPUT_VARIABLE _nm_out
    RESULT_VARIABLE _nm_rc
    ERROR_VARIABLE  _nm_err)
if(NOT _nm_rc EQUAL 0)
    message(FATAL_ERROR
        "check_cvm_freestanding: nm failed (rc=${_nm_rc}): ${_nm_err}")
endif()

string(REPLACE "\n" ";" _nm_lines "${_nm_out}")

set(_undef_syms "")
foreach(_line ${_nm_lines})
    string(STRIP "${_line}" _line)
    if(_line STREQUAL "")
        continue()
    endif()
    # Match either bare "U foo" (from `nm --undefined-only`) or
    # address-prefixed "         U foo" forms. weak-undef "w foo"
    # also indicates a symbol the linker may need to resolve.
    if(_line MATCHES "^[wU] +(.+)$" OR _line MATCHES "^ *U +(.+)$")
        list(APPEND _undef_syms "${CMAKE_MATCH_1}")
    endif()
endforeach()

list(REMOVE_DUPLICATES _undef_syms)

set(_violations "")
foreach(_sym ${_undef_syms})
    set(_allowed FALSE)
    foreach(_e ${_allow_exact})
        if(_sym STREQUAL _e)
            set(_allowed TRUE)
            break()
        endif()
    endforeach()
    if(NOT _allowed)
        foreach(_p ${_allow_prefix})
            string(LENGTH "${_p}" _plen)
            string(LENGTH "${_sym}" _slen)
            if(NOT _slen LESS _plen)
                string(SUBSTRING "${_sym}" 0 ${_plen} _head)
                if(_head STREQUAL _p)
                    set(_allowed TRUE)
                    break()
                endif()
            endif()
        endforeach()
    endif()
    if(NOT _allowed)
        list(APPEND _violations "${_sym}")
    endif()
endforeach()

if(_violations)
    string(REPLACE ";" "\n  " _violations_pretty "${_violations}")
    message(FATAL_ERROR
        "Cortex-M sanity: libcvm.a references symbols outside the "
        "freestanding allowlist:\n  ${_violations_pretty}\n"
        "If a symbol is legitimate compiler-runtime/freestanding ABI, "
        "add it to ${ALLOWLIST}. If it's a libc leak (malloc, printf, "
        "exit, ...), fix the source.")
endif()

list(LENGTH _undef_syms _n)
message(STATUS
    "Cortex-M sanity: libcvm.a clean — ${_n} undefined symbol(s), "
    "all in allowlist")
