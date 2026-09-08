# Copy the compiler's own runtime DLLs next to a freshly linked executable.
#
# aurora_copy_runtime_dlls() handles everything CMake knows as an imported
# target -- SDL3, nod, Dawn. It cannot see the rest: libwinpthread-1.dll,
# zlib1.dll, libbz2-1.dll and libsqlite3-0.dll arrive transitively through the
# MinGW toolchain and through find_library() results that are bare paths, not
# targets, so $<TARGET_RUNTIME_DLLS> never mentions them. They resolve during
# development only because the MSYS2 shell already has C:/msys64/ucrt64/bin on
# PATH, which makes the executable look finished when it is not: launched from
# Explorer, from a plain PowerShell, or on any machine without MSYS2, the loader
# fails before a single instruction of main() runs and Windows reports it as
# exit code 0xC0000135 with no message at all. The window never appears and
# nothing is printed, so it reads as the program silently doing nothing.
#
# Rather than hard-code the list -- which would go stale the next time a
# dependency is added -- walk the real import tables with objdump and copy the
# closure. Run as a script: cmake -P this -DEXE=... -DOBJDUMP=... -DBIN_DIRS=...
#
#   EXE        the linked executable; its directory is the destination
#   OBJDUMP    a binutils objdump that understands PE imports
#   BIN_DIRS   ;-list of directories to source DLLs from (the toolchain's bin)
#   SYSTEM_DIR the Windows system directory, whose DLLs are deliberately skipped

cmake_minimum_required(VERSION 3.25)

if (NOT EXISTS "${EXE}")
    message(FATAL_ERROR "CopyCompilerRuntimeDLLs: no such executable: ${EXE}")
endif ()

cmake_path(GET EXE PARENT_PATH _out_dir)

# Names the OS resolves through the API set schema rather than from disk. They
# are never real files, so looking for them would report false missing entries.
set(_api_set_prefixes "api-ms-win-" "ext-ms-win-")

set(_pending "${EXE}")
set(_seen "")
set(_copied "")

while (_pending)
    list(POP_FRONT _pending _file)

    execute_process(
        COMMAND "${OBJDUMP}" -p "${_file}"
        OUTPUT_VARIABLE _dump
        ERROR_VARIABLE _dump_err
        RESULT_VARIABLE _dump_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if (NOT _dump_rc EQUAL 0)
        # A DLL we cannot read is not fatal: it may be a delay-loaded stub or a
        # format objdump does not handle. Skip it and keep walking the rest.
        continue ()
    endif ()

    string(REGEX MATCHALL "DLL Name: [^\n\r]+" _dep_lines "${_dump}")
    foreach (_line IN LISTS _dep_lines)
        string(REGEX REPLACE "^DLL Name: +" "" _dep "${_line}")
        string(STRIP "${_dep}" _dep)
        if (NOT _dep)
            continue ()
        endif ()

        string(TOLOWER "${_dep}" _dep_lower)
        if (_dep_lower IN_LIST _seen)
            continue ()
        endif ()
        list(APPEND _seen "${_dep_lower}")

        set(_is_api_set FALSE)
        foreach (_prefix IN LISTS _api_set_prefixes)
            if (_dep_lower MATCHES "^${_prefix}")
                set(_is_api_set TRUE)
            endif ()
        endforeach ()
        if (_is_api_set)
            continue ()
        endif ()

        # Already beside the executable: do not copy, but do walk its imports,
        # since a DLL placed here by another rule can pull in more than the
        # executable itself does.
        if (EXISTS "${_out_dir}/${_dep}")
            list(APPEND _pending "${_out_dir}/${_dep}")
            continue ()
        endif ()

        # Shipped by Windows. Copying one would shadow the system copy, because
        # the executable's own directory is searched ahead of the system one.
        if (SYSTEM_DIR AND EXISTS "${SYSTEM_DIR}/${_dep}")
            continue ()
        endif ()

        foreach (_dir IN LISTS BIN_DIRS)
            if (EXISTS "${_dir}/${_dep}")
                file(COPY "${_dir}/${_dep}" DESTINATION "${_out_dir}")
                list(APPEND _copied "${_dep}")
                list(APPEND _pending "${_out_dir}/${_dep}")
                break ()
            endif ()
        endforeach ()
    endforeach ()
endwhile ()

if (_copied)
    list(JOIN _copied " " _copied_str)
    message(STATUS "Copied compiler runtime DLLs: ${_copied_str}")
endif ()
