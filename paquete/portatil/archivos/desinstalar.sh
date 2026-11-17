#!/bin/bash
# Desinstala DeckLink del portátil

DEST="$HOME/.local/share/decklink"
AQUI="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
cp "$AQUI/decklink.svg" /tmp/decklink-icono.svg 2>/dev/null
ICONO=/tmp/decklink-icono.svg
. "$AQUI/dialogos.sh"

dlg_si_no "¿Seguro que quieres desinstalar DeckLink de este portátil?" || exit 0

pkill -x decklink-portatil 2>/dev/null
rm -f "$HOME/.local/share/applications/decklink.desktop" \
      "$HOME/.local/share/icons/hicolor/scalable/apps/decklink.svg" \
      "$HOME/.local/share/icons/hicolor/256x256/apps/decklink.png" \
      "$HOME/.cache/decklink-portatil.log"
quitar_de_escritorio "DeckLink.desktop"
rm -rf "$DEST"

dlg_info "DeckLink se ha desinstalado de este portátil."
