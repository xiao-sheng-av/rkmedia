find_package(PkgConfig QUIET)
pkg_check_modules(PKG_MINILOGER QUIET "libminilogger")
set(MINILOGER_DEFINITIONS ${PKG_MINILOGER_CFLAGS_OTHER})

find_path(MINILOGER_INCLUDE_DIR
    NAMES log.h
    HINTS ${PKG_MINILOGER_INCLUDEDIR} ${PKG_MINILOGER_INCLUDE_DIRS}
    PATH_SUFFIXES minilogger)

find_library(MINILOGER_LIBRARY
    NAMES minilogger
    HINTS ${PKG_MINILOGER_LIBDIR} ${PKG_MINILOGER_LIBRARY_DIRS})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MINILOGER DEFAULT_MSG
    MINILOGER_LIBRARY MINILOGER_INCLUDE_DIR)
mark_as_advanced(MINILOGER_FOUND MINILOGER_INCLUDE_DIR MINILOGER_LIBRARY)

if(NOT TARGET MiniLogger::MiniLogger)
    add_library(MiniLogger::MiniLogger UNKNOWN IMPORTED)
    set_property(TARGET MiniLogger::MiniLogger PROPERTY IMPORTED_LOCATION ${MINILOGER_LIBRARY})
    set_property(TARGET MiniLogger::MiniLogger PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${MINILOGER_INCLUDE_DIR}")
endif()
