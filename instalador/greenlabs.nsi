; Instalador do GreenLabs para Windows.
;
; Um executavel so, sem dependencia nenhuma: o link e estatico (/MT), entao nao
; ha redistributable do Visual C++ para instalar junto. O instalador existe
; para dar atalho no menu iniciar, entrada em "Aplicativos instalados" e um
; desinstalador - nao para copiar bibliotecas.
;
; Instala por USUARIO, em %LOCALAPPDATA%. Sem UAC, sem pedir senha de
; administrador: quem quer jogar e mostrar a tela nao deveria precisar de
; permissao de administrador para isso, e o programa nao escreve em lugar
; nenhum que exija.

Unicode true

!define NOME     "GreenLabs"
!define EMPRESA  "GreenCodes"
!define CHAVE    "GreenLabs"

Name "${NOME}"
OutFile "${SAIDA}"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\${NOME}"
InstallDirRegKey HKCU "Software\${CHAVE}" "InstallDir"
SetCompressor /SOLID lzma
ShowInstDetails hide
ShowUninstDetails hide
BrandingText "${NOME} ${VERSAO}"

!include "MUI2.nsh"

!define MUI_ICON "${ICONE}"
!define MUI_UNICON "${ICONE}"
!define MUI_ABORTWARNING

; Sem a pagina de licenca e sem a de componentes: ha um componente so, e a
; licenca e a mesma do projeto. Cada pagina a mais e um Avancar a mais entre a
; pessoa e o programa aberto.
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_RUN "$INSTDIR\GreenLabs.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Abrir o ${NOME} agora"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "PortugueseBR"

Section "Principal" SecPrincipal
  SectionIn RO

  ; O programa pode estar aberto: instalar por cima de um executavel em uso
  ; falha no meio e deixa a pasta pela metade.
  DetailPrint "Fechando o ${NOME}, se estiver aberto..."
  nsExec::Exec 'taskkill /F /IM GreenLabs.exe'
  Pop $0

  SetOutPath "$INSTDIR"
  File "${EXECUTAVEL}"

  WriteRegStr HKCU "Software\${CHAVE}" "InstallDir" "$INSTDIR"
  WriteUninstaller "$INSTDIR\Desinstalar.exe"

  CreateShortcut "$SMPROGRAMS\${NOME}.lnk" "$INSTDIR\GreenLabs.exe"
  CreateShortcut "$DESKTOP\${NOME}.lnk" "$INSTDIR\GreenLabs.exe"

  ; Entrada em Configuracoes > Aplicativos instalados.
  !define DESINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\${CHAVE}"
  WriteRegStr   HKCU "${DESINST}" "DisplayName"     "${NOME}"
  WriteRegStr   HKCU "${DESINST}" "DisplayVersion"  "${VERSAO}"
  WriteRegStr   HKCU "${DESINST}" "Publisher"       "${EMPRESA}"
  WriteRegStr   HKCU "${DESINST}" "DisplayIcon"     "$INSTDIR\GreenLabs.exe"
  WriteRegStr   HKCU "${DESINST}" "UninstallString" "$INSTDIR\Desinstalar.exe"
  WriteRegStr   HKCU "${DESINST}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKCU "${DESINST}" "NoModify" 1
  WriteRegDWORD HKCU "${DESINST}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  nsExec::Exec 'taskkill /F /IM GreenLabs.exe'
  Pop $0

  Delete "$INSTDIR\GreenLabs.exe"
  Delete "$INSTDIR\Desinstalar.exe"
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\${NOME}.lnk"
  Delete "$DESKTOP\${NOME}.lnk"

  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${CHAVE}"
  DeleteRegKey HKCU "Software\${CHAVE}"

  ; As preferencias e o log NAO sao apagados de proposito: quem desinstala para
  ; instalar de novo espera o endereco do servidor ainda estar la, e o log e a
  ; unica coisa que explica um problema que aconteceu antes.
SectionEnd
