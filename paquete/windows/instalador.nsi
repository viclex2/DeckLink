; Instalador de DeckLink para Windows (NSIS)
Target amd64-unicode
!include "MUI2.nsh"

!define NOMBRE "DeckLink"
!define VERSION "2.14"
!define CLAVE_DESINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\DeckLink"

Name "${NOMBRE}"
OutFile "..\..\dist\DeckLink-Windows-Instalador.exe"
InstallDir "$PROGRAMFILES64\DeckLink"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "DeckLink ${VERSION}"

!define MUI_ICON "..\..\arte\decklink.ico"
!define MUI_UNICON "..\..\arte\decklink.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Instalar DeckLink"
!define MUI_WELCOMEPAGE_TEXT "DeckLink convierte este PC en la pantalla de tu Steam Deck: verás su imagen, oirás su sonido y podrás usar el teclado y el ratón.$\r$\n$\r$\nLa Steam Deck también necesita tener instalado DeckLink (la versión para Steam Deck).$\r$\n$\r$\nPulsa Siguiente para continuar."
!define MUI_FINISHPAGE_TITLE "DeckLink está instalado"
!define MUI_FINISHPAGE_TEXT "Para usarlo:$\r$\n$\r$\n1. Conecta la Steam Deck a este PC con un cable USB (o activa el wifi en la Deck).$\r$\n2. Abre DeckLink desde el icono del escritorio.$\r$\n$\r$\nPara salir de la pantalla completa: Ctrl + Alt + Mayús + Q"
!define MUI_FINISHPAGE_RUN "$INSTDIR\DeckLink.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Abrir DeckLink ahora"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Spanish"

VIProductVersion "2.14.0.0"
VIAddVersionKey /LANG=${LANG_SPANISH} "ProductName" "DeckLink"
VIAddVersionKey /LANG=${LANG_SPANISH} "FileDescription" "Instalador de DeckLink"
VIAddVersionKey /LANG=${LANG_SPANISH} "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=${LANG_SPANISH} "LegalCopyright" "MIT"

Section "DeckLink"
  SetShellVarContext all
  ; cerrar una versión anterior abierta
  nsExec::Exec 'taskkill /IM DeckLink.exe /F'
  SetOutPath "$INSTDIR"
  File "..\..\build\DeckLink.exe"
  File "..\..\arte\decklink.ico"
  File /oname=LEEME.txt "LEEME-windows.txt"
  WriteUninstaller "$INSTDIR\Desinstalar DeckLink.exe"

  CreateShortcut "$DESKTOP\DeckLink.lnk" "$INSTDIR\DeckLink.exe" "" "$INSTDIR\decklink.ico"
  CreateDirectory "$SMPROGRAMS\DeckLink"
  CreateShortcut "$SMPROGRAMS\DeckLink\DeckLink.lnk" "$INSTDIR\DeckLink.exe" "" "$INSTDIR\decklink.ico"
  CreateShortcut "$SMPROGRAMS\DeckLink\Desinstalar DeckLink.lnk" "$INSTDIR\Desinstalar DeckLink.exe"

  ; permitir que DeckLink reciba los avisos de la Deck por wifi (sin preguntar)
  nsExec::Exec 'netsh advfirewall firewall delete rule name="DeckLink"'
  nsExec::Exec 'netsh advfirewall firewall add rule name="DeckLink" dir=in action=allow program="$INSTDIR\DeckLink.exe" enable=yes profile=any'

  WriteRegStr HKLM "${CLAVE_DESINST}" "DisplayName" "DeckLink"
  WriteRegStr HKLM "${CLAVE_DESINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${CLAVE_DESINST}" "Publisher" "DeckLink"
  WriteRegStr HKLM "${CLAVE_DESINST}" "DisplayIcon" "$INSTDIR\decklink.ico"
  WriteRegStr HKLM "${CLAVE_DESINST}" "UninstallString" '"$INSTDIR\Desinstalar DeckLink.exe"'
  WriteRegDWORD HKLM "${CLAVE_DESINST}" "NoModify" 1
  WriteRegDWORD HKLM "${CLAVE_DESINST}" "NoRepair" 1

  ; actualización automática (instalador lanzado por DeckLink con /S): volver a abrirlo
  IfSilent 0 +2
    Exec '"$INSTDIR\DeckLink.exe"'
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  nsExec::Exec 'taskkill /IM DeckLink.exe /F'
  nsExec::Exec 'netsh advfirewall firewall delete rule name="DeckLink"'
  Delete "$DESKTOP\DeckLink.lnk"
  RMDir /r "$SMPROGRAMS\DeckLink"
  Delete "$INSTDIR\DeckLink.exe"
  Delete "$INSTDIR\decklink.ico"
  Delete "$INSTDIR\LEEME.txt"
  Delete "$INSTDIR\Desinstalar DeckLink.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "${CLAVE_DESINST}"
SectionEnd
