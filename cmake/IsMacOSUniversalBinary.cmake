function(IsMacOSUniversalBinary LIBRARY_NAME RESULT_VAL)
    if (NOT BUILD_OSX_UNIVERSAL)
        # Library is always usable for non-macOS / non-universal binaries
        set(${RESULT_VAL} 1 PARENT_SCOPE)
    else()
        # Get architecture list for given library
        execute_process(COMMAND otool -L ${LIBRARY_NAME}
                        OUTPUT_VARIABLE OTOOL_RESULT)
        string(FIND "${OTOOL_RESULT}" "x86_64" INTEL_FOUND)
        string(FIND "${OTOOL_RESULT}" "arm64" ARM_FOUND)

        # Return result to caller
        if ((INTEL_FOUND GREATER -1) AND (ARM_FOUND GREATER -1))
            set(${RESULT_VAL} 1 PARENT_SCOPE)
        else()
            set(${RESULT_VAL} 0 PARENT_SCOPE)
        endif()
    endif()
endfunction()
