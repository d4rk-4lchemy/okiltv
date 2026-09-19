Unicode true
!include "nsDialogs.nsh"
!include "LogicLib.nsh"

!define MUI_FINISHPAGE_RUN_TEXT "Run application"
Var OkiltvExistingDir
Var OkiltvExistingUninstaller
Var OkiltvNewDir
Var OkiltvMode
Var OkiltvDialog
Var OkiltvUpdateRadio
Var OkiltvCleanRadio

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
  !insertmacro MUI_HEADER_TEXT "Istniejąca instalacja" "Wybierz sposób instalacji OKILTV."
  nsDialogs::Create 1018
  Pop $OkiltvDialog
  ${If} $OkiltvDialog == error
    Abort
  ${EndIf}
  ${NSD_CreateLabel} 0 0 100% 36u "Znaleziono OKILTV w folderze:$\r$\n$OkiltvExistingDir"
  Pop $0
  ${NSD_CreateRadioButton} 0 45u 100% 18u "Aktualizuj obecną instalację"
  Pop $OkiltvUpdateRadio
  ${NSD_CreateLabel} 12u 65u 95% 24u "Pliki aplikacji zostaną zaktualizowane w obecnym folderze."
  Pop $0
  ${NSD_CreateRadioButton} 0 92u 100% 18u "Nowa, czysta instalacja"
  Pop $OkiltvCleanRadio
  ${NSD_CreateLabel} 12u 112u 95% 36u "Usuń poprzednią wersję i wybierz folder instalacji (może być ten sam). Źródła i ustawienia użytkownika zostaną zachowane."
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
  MessageBox MB_OK|MB_ICONSTOP "Nie udało się usunąć poprzedniej instalacji. Zamknij OKILTV i ponów instalację." /SD IDOK
  SetErrorLevel 1
  Abort
FunctionEnd
