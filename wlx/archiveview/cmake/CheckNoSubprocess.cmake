# Build-time guard: archiveview must never spawn a process.
#
# The plugin this one replaces ran
#   /bin/sh -c "7z l <filename> | pcregrep ..."
# with the previewed file's name spliced into the command string and only
# spaces and single quotes escaped. Opening a file named x$(...).zip in the
# file manager executed its contents.
#
# The fix is architectural — the archive comment comes from parsing the file,
# not from scraping another program's output — so the absence of any
# subprocess API is a property worth enforcing mechanically. Reintroducing one
# should fail the build, not wait for a reviewer to notice.

file(GLOB_RECURSE SOURCES "${SOURCE_DIR}/*.cpp" "${SOURCE_DIR}/*.h")

set(FORBIDDEN
    "QProcess"
    "[^a-zA-Z_]system[ \t]*\\("
    "popen[ \t]*\\("
    "execv"
    "execl"
    "fork[ \t]*\\("
    "/bin/sh"
)

set(VIOLATIONS "")

foreach(FILE ${SOURCES})
    file(STRINGS "${FILE}" LINES)
    set(LINE_NUMBER 0)
    foreach(LINE ${LINES})
        math(EXPR LINE_NUMBER "${LINE_NUMBER} + 1")
        # Skip comment lines — this file's own rationale is quoted in them.
        if(LINE MATCHES "^[ \t]*(//|\\*|/\\*)")
            continue()
        endif()
        foreach(PATTERN ${FORBIDDEN})
            if(LINE MATCHES "${PATTERN}")
                list(APPEND VIOLATIONS "${FILE}:${LINE_NUMBER}: ${LINE}")
            endif()
        endforeach()
    endforeach()
endforeach()

if(VIOLATIONS)
    string(REPLACE ";" "\n  " REPORT "${VIOLATIONS}")
    message(FATAL_ERROR
        "archiveview: subprocess usage is forbidden in this plugin.\n  ${REPORT}")
endif()
