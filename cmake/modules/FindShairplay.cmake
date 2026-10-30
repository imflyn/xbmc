#.rst:
# FindShairplay
# -------------
# Finds the Shairplay library
#
# This will define the following target:
#
#   Shairplay::Shairplay - The Shairplay library

if(NOT TARGET ${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME})

  find_path(SHAIRPLAY_INCLUDE_DIR shairplay/raop.h
                                  HINTS ${DEPENDS_PATH}/include)
  set(_shairplay_names shairplay libshairplay)
  if(CORE_SYSTEM_NAME STREQUAL darwin_embedded)
    list(PREPEND _shairplay_names libshairplay.a)
  endif()
  find_library(SHAIRPLAY_LIBRARY NAMES ${_shairplay_names}
                                 HINTS ${DEPENDS_PATH}/lib)

  if(NOT VERBOSE_FIND)
     set(${CMAKE_FIND_PACKAGE_NAME}_FIND_QUIETLY TRUE)
   endif()

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(Shairplay
                                    REQUIRED_VARS SHAIRPLAY_LIBRARY SHAIRPLAY_INCLUDE_DIR)

  if(SHAIRPLAY_FOUND)
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_INCLUDES ${SHAIRPLAY_INCLUDE_DIR})
    set(CMAKE_REQUIRED_LIBRARIES ${SHAIRPLAY_LIBRARIES})
    check_c_source_compiles("#include <shairplay/raop.h>

                             int main()
                             {
                               struct raop_callbacks_s foo;
                               foo.cls;
                               return 0;
                             }
                            " HAVE_SHAIRPLAY_CALLBACK_CLS)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_LIBRARIES)

    if(HAVE_SHAIRPLAY_CALLBACK_CLS)
      add_library(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} UNKNOWN IMPORTED)
      set_target_properties(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} PROPERTIES
                                                                       IMPORTED_LOCATION "${SHAIRPLAY_LIBRARY}"
                                                                       INTERFACE_INCLUDE_DIRECTORIES "${SHAIRPLAY_INCLUDE_DIR}"
                                                                       INTERFACE_COMPILE_DEFINITIONS HAS_AIRTUNES)
    endif()
  endif()
endif()
