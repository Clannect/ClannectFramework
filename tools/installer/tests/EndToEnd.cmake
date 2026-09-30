# The installer as people run it: an offline installer made by appending a
# package to the program, a silent install, then the uninstall command that
# Windows' Installed apps runs. Arguments: -DINSTALLER=<program>
# -DPACKAGE=<zip> -DWORK=<folder> [-DEMULATOR=wine].

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
get_filename_component(extension "${INSTALLER}" LAST_EXT)
set(offline "${WORK}/offline-installer${extension}")
execute_process(COMMAND ${CMAKE_COMMAND} -E cat "${INSTALLER}" "${PACKAGE}" OUTPUT_FILE "${offline}" RESULT_VARIABLE cat)
if(NOT cat EQUAL 0)
    message(FATAL_ERROR "could not make the offline installer")
endif()
file(CHMOD "${offline}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)

set(root "${WORK}/root")
execute_process(COMMAND ${EMULATOR} "${offline}" --silent --root "${root}" RESULT_VARIABLE installed)
if(NOT installed EQUAL 0 OR NOT EXISTS "${root}/0.9.0/lib/cmake/ClannectFramework/ClannectFrameworkConfig.cmake")
    message(FATAL_ERROR "the offline installer did not install (exit ${installed})")
endif()
file(GLOB tool "${root}/maintenance/*")
file(SIZE "${tool}" tool_size)
file(SIZE "${INSTALLER}" program_size)
if(NOT tool_size EQUAL program_size)
    message(FATAL_ERROR "the maintenance tool is not the installer without its package (${tool_size} vs ${program_size})")
endif()

execute_process(COMMAND ${EMULATOR} "${tool}" --uninstall "${root}/0.9.0" --silent RESULT_VARIABLE removed)
if(NOT removed EQUAL 0 OR EXISTS "${root}/0.9.0")
    message(FATAL_ERROR "uninstalling failed (exit ${removed})")
endif()
message(STATUS "offline install and uninstall: OK")
