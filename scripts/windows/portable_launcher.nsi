Unicode True
RequestExecutionLevel user
SilentInstall silent
AutoCloseWindow true
ShowInstDetails nevershow

!include "FileFunc.nsh"

!ifndef APP_DIR
  !error "APP_DIR is required"
!endif

!ifndef APP_VERSION
  !error "APP_VERSION is required"
!endif

!ifndef OUTPUT_FILE
  !error "OUTPUT_FILE is required"
!endif

!ifndef APP_ICON
  !error "APP_ICON is required"
!endif

Name "OKILTV Portable"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\OKILTVPortable\runtime\${APP_VERSION}"
Icon "${APP_ICON}"

Var LauncherDir
Var PortableBootstrap
Var RuntimeRoot
Var FindHandle
Var FindName

Function .onInit
  ${GetParent} "$EXEPATH" $LauncherDir
  StrCpy $PortableBootstrap "$LauncherDir\OKILTV-portable.json"
  StrCpy $RuntimeRoot "$LOCALAPPDATA\OKILTVPortable\runtime"
FunctionEnd

Section
  IfFileExists "$PortableBootstrap" bootstrap_done 0
  FileOpen $0 "$PortableBootstrap" w
  IfErrors bootstrap_done
  FileWrite $0 "{$\r$\n  $\"schemaVersion$\": 1,$\r$\n  $\"dataRootOverride$\": $\"$\"$\r$\n}$\r$\n"
  FileClose $0

bootstrap_done:
  IfFileExists "$RuntimeRoot\*.*" cleanup_start cleanup_done

cleanup_start:
  FindFirst $FindHandle $FindName "$RuntimeRoot\*"
cleanup_loop:
  StrCmp $FindName "" cleanup_end
  StrCmp $FindName "." cleanup_next
  StrCmp $FindName ".." cleanup_next
  RMDir /r "$RuntimeRoot\$FindName"
cleanup_next:
  FindNext $FindHandle $FindName
  IfErrors cleanup_end
  Goto cleanup_loop
cleanup_end:
  FindClose $FindHandle

cleanup_done:
  SetOutPath "$INSTDIR"
  File /r "${APP_DIR}\*.*"
  Exec '"$INSTDIR\OKILTV.exe" --portable-bootstrap "$PortableBootstrap"'
SectionEnd
