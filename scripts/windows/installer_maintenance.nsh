Unicode true
!include "nsDialogs.nsh"
!include "LogicLib.nsh"

!define MUI_FINISHPAGE_RUN_TEXT "Run application"
!define MUI_FINISHPAGE_RUN_FUNCTION OkiltvLaunchApplication
Var OkiltvExistingDir
Var OkiltvExistingUninstaller
Var OkiltvNewDir
Var OkiltvMode
Var OkiltvDialog
Var OkiltvUpdateRadio
Var OkiltvCleanRadio

Function OkiltvLaunchApplication
  Push $0
  Push $1
  Push $2
  Push $3
  Push $4
  Push $5
  Push $6
  ; NSIS is a 32-bit process, including when installing the 64-bit application.
  ; STARTUPINFOW (68 bytes): STARTF_USESHOWWINDOW, SW_SHOWNORMAL.
  System::Call '*(i 68, p 0, p 0, p 0, i 0, i 0, i 0, i 0, i 0, i 0, i 0, i 1, &i2 1, &i2 0, p 0, p 0, p 0, p 0) p.r0'
  System::Call '*(p 0, p 0, i 0, i 0) p.r1'
  ; CREATE_SUSPENDED lets the foreground installer grant activation permission
  ; to this exact child before it creates its first window.
  System::Call 'kernel32::CreateProcessW(w "$INSTDIR\OKILTV.exe", p 0, p 0, p 0, i 0, i 4, p 0, w "$INSTDIR", p r0, p r1) i.r2'
  ${If} $2 <> 0
    System::Call '*$1(p .r3, p .r4, i .r5, i .r6)'
    System::Call 'user32::AllowSetForegroundWindow(i r5) i.r2'
    ${If} $2 == 0
      DetailPrint "Windows declined foreground activation permission for OKILTV."
    ${EndIf}
    System::Call 'kernel32::ResumeThread(p r4) i.r2'
    ${If} $2 == -1
      ; Do not leave a suspended child behind if Windows cannot resume it.
      System::Call 'kernel32::TerminateProcess(p r3, i 1)'
      MessageBox MB_OK|MB_ICONEXCLAMATION "Could not start OKILTV. Please launch it from the Start menu."
    ${EndIf}
    System::Call 'kernel32::CloseHandle(p r4)'
    System::Call 'kernel32::CloseHandle(p r3)'
  ${Else}
    MessageBox MB_OK|MB_ICONEXCLAMATION "Could not start OKILTV. Please launch it from the Start menu."
  ${EndIf}
  System::Free $1
  System::Free $0
  Pop $6
  Pop $5
  Pop $4
  Pop $3
  Pop $2
  Pop $1
  Pop $0
FunctionEnd

; Existing CPack installers register in the default (32-bit) registry view,
; even though the application itself is 64-bit. Keep that registration stable.
Function OkiltvDetectInstallation
  StrCmp $OkiltvMode "" 0 done
  StrCpy $OkiltvMode "new"
  StrCpy $OkiltvNewDir $INSTDIR
  ReadRegStr $OkiltvExistingDir SHCTX "Software\OKILTV\OKILTV" ""
  StrCmp $OkiltvExistingDir "" done
  StrCpy $OkiltvExistingUninstaller "$OkiltvExistingDir\Uninstall.exe"
  IfFileExists "$OkiltvExistingUninstaller" found
  ; Older hand-written CPack configs left CPACK_NSIS_UNINSTALL_NAME empty.
  StrCpy $OkiltvExistingUninstaller "$OkiltvExistingDir\.exe"
  IfFileExists "$OkiltvExistingUninstaller" found
  IfFileExists "$OkiltvExistingDir\OKILTV.exe" found
  Goto done
found:
  StrCpy $OkiltvMode "update"
  StrCpy $INSTDIR $OkiltvExistingDir
done:
FunctionEnd

Function OkiltvFinishInstall
  ; Retire the legacy unnamed uninstaller only after its replacement exists.
  ${If} $OkiltvMode == "update"
  ${AndIf} $OkiltvExistingUninstaller == "$INSTDIR\.exe"
    Delete "$INSTDIR\.exe"
  ${EndIf}
FunctionEnd

Function OkiltvMaintenanceCreate
  Call OkiltvDetectInstallation
  StrCmp $OkiltvMode "new" 0 +2
    Abort
  !insertmacro MUI_HEADER_TEXT "Existing installation" "Choose how to install OKILTV."
  nsDialogs::Create 1018
  Pop $OkiltvDialog
  ${If} $OkiltvDialog == error
    Abort
  ${EndIf}
  ${NSD_CreateLabel} 0 0 100% 36u "OKILTV was found in the following folder:$\r$\n$OkiltvExistingDir"
  Pop $0
  ${NSD_CreateRadioButton} 0 45u 100% 18u "Update existing installation"
  Pop $OkiltvUpdateRadio
  ${NSD_CreateLabel} 12u 65u 95% 24u "Application files will be updated in the existing folder."
  Pop $0
  ${NSD_CreateRadioButton} 0 92u 100% 18u "New, clean installation"
  Pop $OkiltvCleanRadio
  ${NSD_CreateLabel} 12u 112u 95% 36u "Remove the previous version and choose an installation folder (it may be the same). User sources and settings will be preserved."
  Pop $0
  ${If} $OkiltvMode == "clean"
    ${NSD_Check} $OkiltvCleanRadio
  ${Else}
    ${NSD_Check} $OkiltvUpdateRadio
  ${EndIf}
  nsDialogs::Show
FunctionEnd

Function OkiltvMaintenanceLeave
  ${NSD_GetState} $OkiltvUpdateRadio $0
  ${If} $0 == ${BST_CHECKED}
    ${If} $OkiltvMode == "clean"
      StrCpy $OkiltvNewDir $INSTDIR
    ${EndIf}
    StrCpy $OkiltvMode "update"
    StrCpy $INSTDIR $OkiltvExistingDir
  ${Else}
    ${If} $OkiltvMode != "clean"
      StrCpy $INSTDIR $OkiltvNewDir
    ${EndIf}
    StrCpy $OkiltvMode "clean"
  ${EndIf}
FunctionEnd

Function OkiltvDirectoryPre
  ${If} $OkiltvMode == "update"
    StrCpy $INSTDIR $OkiltvExistingDir
    Abort
  ${EndIf}
FunctionEnd

Function OkiltvPrepareInstall
  ; Silent installations also detect the old path and default to updating it.
  Call OkiltvDetectInstallation
  ${If} $OkiltvMode == "update"
    StrCpy $INSTDIR $OkiltvExistingDir
  ${ElseIf} $OkiltvMode == "clean"
    ; Run only after Install is clicked. Copy the old uninstaller outside its
    ; directory, wait for actual completion, and never recursively delete a
    ; user-selected directory or the user's AppData.
    InitPluginsDir
    SetOutPath "$PLUGINSDIR"
    ClearErrors
    CopyFiles /SILENT "$OkiltvExistingUninstaller" "$PLUGINSDIR\OkiltvPreviousUninstall.exe"
    IfErrors failed
    ExecWait '"$PLUGINSDIR\OkiltvPreviousUninstall.exe" /S _?=$OkiltvExistingDir' $0
    IfErrors failed
    StrCmp $0 0 0 failed
    IfFileExists "$OkiltvExistingDir\OKILTV.exe" failed
    ReadRegStr $0 SHCTX "Software\OKILTV\OKILTV" ""
    StrCmp $0 "" 0 failed
  ${EndIf}
  SetOutPath "$INSTDIR"
  Return
failed:
  MessageBox MB_OK|MB_ICONSTOP "Failed to remove the previous installation. Close OKILTV and try again." /SD IDOK
  SetErrorLevel 1
  Abort
FunctionEnd
