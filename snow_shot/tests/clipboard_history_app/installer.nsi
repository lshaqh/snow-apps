; SPDX-License-Identifier: GPL-3.0-or-later
; Minimal NSIS installer for the portable Win+V clipboard-history build.
; OUTFILE and DISTDIR are supplied by the CI workflow with /D switches.

Unicode true

!define APPNAME "Snow Shot Clipboard History"
!define EXENAME "snow_shot_clipboard_history.exe"

!ifndef OUTFILE
  !define OUTFILE "snow-shot-clipboard-history-setup.exe"
!endif
!ifndef DISTDIR
  !define DISTDIR "dist"
!endif

Name "${APPNAME}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\SnowShotClipboardHistory"
RequestExecutionLevel user
SetCompressor /SOLID lzma

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "Install"
  SetOutPath "$INSTDIR"
  File /r "${DISTDIR}\*.*"

  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${EXENAME}"
  CreateShortcut "$SMSTARTUP\${APPNAME}.lnk" "$INSTDIR\${EXENAME}"

  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SnowShotClipboardHistory" \
                   "DisplayName" "${APPNAME}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SnowShotClipboardHistory" \
                   "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SnowShotClipboardHistory" \
                   "InstallLocation" "$INSTDIR"

  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\${APPNAME}.lnk"
  Delete "$SMSTARTUP\${APPNAME}.lnk"
  Delete "$INSTDIR\uninstall.exe"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SnowShotClipboardHistory"
  RMDir /r "$INSTDIR"
SectionEnd
