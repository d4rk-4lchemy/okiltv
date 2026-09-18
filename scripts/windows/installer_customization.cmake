# Keep CPack's file manifest, registry and uninstaller machinery. Inject only
# the maintenance page and directory-page hook, for which CPack has no API.
file(READ "${CMAKE_ROOT}/Modules/Internal/CPack/NSIS.template.in" _okiltv_template)
set(_okiltv_directory_page "  !insertmacro MUI_PAGE_DIRECTORY")
string(FIND "${_okiltv_template}" "${_okiltv_directory_page}" _okiltv_page_offset)
if(_okiltv_page_offset EQUAL -1)
  message(FATAL_ERROR "CPack NSIS template changed: cannot insert the OKILTV maintenance page")
endif()
string(REPLACE "${_okiltv_directory_page}" [=[
  Page custom OkiltvMaintenanceCreate OkiltvMaintenanceLeave
  !define MUI_PAGE_CUSTOMFUNCTION_PRE OkiltvDirectoryPre
  !insertmacro MUI_PAGE_DIRECTORY]=] _okiltv_template "${_okiltv_template}")
list(GET CPACK_INSTALLED_DIRECTORIES 0 _okiltv_stage_dir)
get_filename_component(_okiltv_stage_parent "${_okiltv_stage_dir}" DIRECTORY)
set(_okiltv_template_dir "${_okiltv_stage_parent}/nsis-template")
file(MAKE_DIRECTORY "${_okiltv_template_dir}")
file(WRITE "${_okiltv_template_dir}/NSIS.template.in" "${_okiltv_template}")
list(PREPEND CMAKE_MODULE_PATH "${_okiltv_template_dir}")
file(READ "${CMAKE_CURRENT_LIST_DIR}/installer_maintenance.nsh" CPACK_NSIS_DEFINES)
set(CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS "Call OkiltvPrepareInstall")
