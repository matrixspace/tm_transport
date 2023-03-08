file(TO_CMAKE_PATH "$ENV{TM_KIT_HOME}" TM_KIT_HOME)
file(TO_CMAKE_PATH "$ENV{TM_KIT_DIR}" TM_KIT_DIR)

set(TMInfra_INCLUDE_SEARCH_PATHS
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

set(TMInfra_LIB_SEARCH_PATHS
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

FIND_PATH(TMInfra_INCLUDE_DIR NAMES tm_kit/infra/Environments.hpp HINTS ${TMInfra_INCLUDE_SEARCH_PATHS})

set(TMInfra_NAMES tm_kit_infra tm_infra tm_infra.a tm_infra.lib)
set(TMInfra_NAMES_DEBUG tm_kit_infra_debug tm_infra_debug tm_infra_d)
if(NOT TMInfra_LIBRARY)
    find_library(TMInfra_LIBRARY_RELEASE NAMES ${TMInfra_NAMES} HINTS ${TMInfra_LIB_SEARCH_PATHS} NAMES_PER_DIR)
    find_library(TMInfra_LIBRARY_DEBUG NAMES ${TMInfra_NAMES_DEBUG} HINTS ${TMInfra_LIB_SEARCH_PATHS} NAMES_PER_DIR)
    include(SelectLibraryConfigurations)
    select_library_configurations(TMInfra)
    mark_as_advanced(TMInfra_LIBRARY_RELEASE TMInfra_LIBRARY_DEBUG)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(TMInfra REQUIRED_VARS TMInfra_LIBRARY TMInfra_INCLUDE_DIR)