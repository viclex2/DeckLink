#!/bin/bash
# Desinstala DeckLink de la Steam Deck

DEST="$HOME/.local/share/decklink"
AQUI="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
ICONO="$AQUI/decklink.svg"
. "$AQUI/dialogos.sh"
. "$AQUI/permisos.sh"

dlg_si_no "¿Seguro que quieres desinstalar DeckLink de esta Steam Deck?" || exit 0
obtener_root || { dlg_error "Sin la contraseña no se puede desinstalar."; exit 1; }

systemctl --user disable --now decklink.service app-io.github.viclex2.decklink.service >/dev/null 2>&1
rm -f "$HOME/.config/systemd/user/decklink.service" "$HOME/.config/systemd/user/app-io.github.viclex2.decklink.service"
systemctl --user daemon-reload

TMP=$(mktemp)
cat > "$TMP" <<'EOF'
systemctl disable --now decklink-red.service
rm -f /etc/systemd/system/decklink-red.service /etc/NetworkManager/conf.d/decklink.conf \
      /etc/udev/rules.d/60-decklink-uinput.rules /etc/modules-load.d/decklink.conf
systemctl daemon-reload
udevadm control --reload
nmcli general reload conf >/dev/null 2>&1
true
EOF
root bash "$TMP" >/dev/null 2>&1
rm -f "$TMP"

rm -f "$HOME/.local/share/applications/decklink.desktop" "$HOME/.local/share/applications/io.github.viclex2.decklink.desktop"
quitar_de_escritorio "DeckLink.desktop"
rm -rf "$HOME/.config/decklink"
cp "$ICONO" /tmp/decklink-icono.svg 2>/dev/null
ICONO=/tmp/decklink-icono.svg
rm -rf "$DEST"

dlg_info "DeckLink se ha desinstalado.

Si quieres, puedes volver a poner «USB Dual-Role Device» en XHCI en la BIOS (no es obligatorio)."
