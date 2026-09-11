# Applies PATCH_FILE to the current directory (a git checkout) unless it has
# already been applied. Needed because FetchContent's PATCH_COMMAND re-runs on
# every CMake re-configure that touches this dependency, not just the first
# populate -- a plain `git apply` would fail on the second run since the patch
# is already in the tree.
#
# Usage: cmake -DPATCH_FILE=<path> -P apply_if_needed.cmake
# (run with the git checkout as the working directory)

execute_process(
    COMMAND git apply --reverse --check "${PATCH_FILE}"
    RESULT_VARIABLE ALREADY_APPLIED
    OUTPUT_QUIET
    ERROR_QUIET
)

if (NOT ALREADY_APPLIED EQUAL 0)
    execute_process(
        COMMAND git apply --whitespace=fix "${PATCH_FILE}"
        RESULT_VARIABLE APPLY_RESULT
    )
    if (NOT APPLY_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to apply patch: ${PATCH_FILE}")
    endif()
endif()
