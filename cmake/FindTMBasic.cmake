file(TO_CMAKE_PATH "$ENV{TM_KIT_HOME}" TM_KIT_HOME)
file(TO_CMAKE_PATH "$ENV{TM_KIT_DIR}" TM_KIT_DIR)

set(TMBasic_INCLUDE_SEARCH_PATHS
  ${TM_KIT_HOME}
  ${TM_KIT_HOME}/include
  ${TM_KIT_DIR}
  ${TM_KIT_DIR}/include
  /usr/include
  /usr/include/tm_kit
  /usr/local/include
  /usr/local/include/tm_kit
  /opt/tm_kit/include
)

set(TMBasic_LIB_SEARCH_PATHS
  ${TM_KIT_HOME}
  ${TM_KIT_HOME}/lib
  ${TM_KIT_DIR}
  ${TM_KIT_DIR}/lib
  /usr/lib
  /usr/lib/tm_kit
  /usr/local/lib
  /usr/local/lib/tm_kit
  /opt/tm_kit/lib
)

FIND_PATH(TMBasic_INCLUDE_DIR NAMES tm_kit/basic/ByteData.hpp HINTS ${TMBasic_INCLUDE_SEARCH_PATHS})

set(TMBasic_NAMES tm_kit_basic tm_basic tm_basic.a tm_basic.lib)
set(TMBasic_NAMES_DEBUG tm_kit_basic_debug tm_basic_debug tm_basic_d)
if(NOT TMBasic_LIBRARY)
    find_library(TMBasic_LIBRARY_RELEASE NAMES ${TMBasic_NAMES} HINTS ${TMBasic_LIB_SEARCH_PATHS} NAMES_PER_DIR)
    find_library(TMBasic_LIBRARY_DEBUG NAMES ${TMBasic_NAMES_DEBUG} HINTS ${TMBasic_LIB_SEARCH_PATHS} NAMES_PER_DIR)
    include(SelectLibraryConfigurations)
    select_library_configurations(TMBasic)
    mark_as_advanced(TMBasic_LIBRARY_RELEASE TMBasic_LIBRARY_DEBUG)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(TMBasic REQUIRED_VARS TMBasic_LIBRARY TMBasic_INCLUDE_DIR)