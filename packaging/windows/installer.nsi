; Origin's Windows installer (NSIS 3): for the current user, no administrator prompt, into %LOCALAPPDATA%\Programs\Origin.
; Built by packaging/windows/pack.sh:  makensis -DVERSION=x.y.z -DSRC=<staged folder> -DICON=<.ico> -DOUT=<setup.exe>
; The app's updater runs it as  setup.exe /S /RELAUNCH : silent, and Origin opens again when it is done.
; Settings, sign-in and caches live in %LOCALAPPDATA%\tv.stremioorigin.app and are never touched by an update.
Unicode true
ManifestDPIAware true
RequestExecutionLevel user
SetCompressor /SOLID lzma
!define APPID "tv.stremioorigin.app"
!define NAME "Origin"
!define UNKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPID}"
!include "MUI2.nsh"
!include "FileFunc.nsh"

Name "${NAME}"
OutFile "${OUT}"
InstallDir "$LOCALAPPDATA\Programs\${NAME}"
BrandingText " "
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${NAME}"
VIAddVersionKey "FileDescription" "${NAME} setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "GPL-3.0"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_FINISHPAGE_RUN "$INSTDIR\Origin.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Open ${NAME}"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var relaunch

Function .onInit
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/RELAUNCH" $1
  StrCpy $relaunch 0
  IfErrors +2
    StrCpy $relaunch 1
FunctionEnd

; Origin still open (an update starts this installer, then the app closes): wait until its files can be replaced
!macro WaitForApp
  StrCpy $2 0
  wait_loop:
    IfFileExists "$INSTDIR\Origin.exe" 0 wait_done
    ClearErrors
    FileOpen $3 "$INSTDIR\Origin.exe" a
    IfErrors 0 wait_open
    IntOp $2 $2 + 1
    IntCmp $2 150 wait_busy
    Sleep 200
    Goto wait_loop
  wait_busy:
    IfSilent 0 +2
      Abort
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "${NAME} is still open. Close it, then choose Retry." IDRETRY +2
      Abort
    StrCpy $2 0
    Goto wait_loop
  wait_open:
    FileClose $3
  wait_done:
!macroend

Section "${NAME}"
  SetOutPath "$INSTDIR"
  !insertmacro WaitForApp
  RMDir /r "$INSTDIR\assets"
  RMDir /r "$INSTDIR\licenses"
  RMDir /r "$INSTDIR\fallback"
  File /r "${SRC}/*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortCut "$SMPROGRAMS\${NAME}.lnk" "$INSTDIR\Origin.exe"
  IfSilent +2
    CreateShortCut "$DESKTOP\${NAME}.lnk" "$INSTDIR\Origin.exe"
  WriteRegStr HKCU "${UNKEY}" "DisplayName" "${NAME}"
  WriteRegStr HKCU "${UNKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNKEY}" "DisplayIcon" "$INSTDIR\Origin.exe"
  WriteRegStr HKCU "${UNKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNKEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegStr HKCU "${UNKEY}" "URLInfoAbout" "https://github.com/betterstremio/origin"
  WriteRegDWORD HKCU "${UNKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNKEY}" "EstimatedSize" "$0"
  StrCmp $relaunch 1 0 +2
    Exec '"$INSTDIR\Origin.exe"'
SectionEnd

; only what the installer put here (never a whole folder it did not make)
Section "Uninstall"
  !insertmacro WaitForApp
  Delete "$INSTDIR\Origin.exe"
  Delete "$INSTDIR\libEGL.dll"
  Delete "$INSTDIR\libGLESv2.dll"
  Delete "$INSTDIR\libmpv-2.dll"
  Delete "$INSTDIR\libsbengine.dll"
  Delete "$INSTDIR\fallback\vulkan-1.dll"
  RMDir "$INSTDIR\fallback"
  Delete "$INSTDIR\appinfo.json"
  RMDir /r "$INSTDIR\assets"
  RMDir /r "$INSTDIR\licenses"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\${NAME}.lnk"
  Delete "$DESKTOP\${NAME}.lnk"
  DeleteRegKey HKCU "${UNKEY}"
  IfSilent un_done
  MessageBox MB_YESNO|MB_DEFBUTTON2 "Also remove your settings, sign-in and caches from this PC?" IDNO un_done
    RMDir /r "$LOCALAPPDATA\${APPID}"
  un_done:
SectionEnd
