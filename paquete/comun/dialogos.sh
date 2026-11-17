# dialogos.sh — ventanas sencillas para los instaladores de DeckLink.
# Usa kdialog (KDE) o zenity (GNOME); si no hay ninguno, notificaciones.
# Antes de cargarlo, define ICONO con la ruta del logo.

_hay() { command -v "$1" >/dev/null 2>&1; }

dlg_info() {
    if _hay kdialog; then kdialog --title "DeckLink" --icon "$ICONO" --msgbox "$1"
    elif _hay zenity; then zenity --info --title="DeckLink" --icon-name=dialog-information --width=460 --text="$1"
    else notify-send -i "$ICONO" "DeckLink" "$1" 2>/dev/null || echo "$1"; fi
}

dlg_error() {
    if _hay kdialog; then kdialog --title "DeckLink" --icon "$ICONO" --error "$1"
    elif _hay zenity; then zenity --error --title="DeckLink" --width=460 --text="$1"
    else notify-send -u critical -i "$ICONO" "DeckLink" "$1" 2>/dev/null || echo "$1" >&2; fi
}

dlg_si_no() {
    if _hay kdialog; then kdialog --title "DeckLink" --icon "$ICONO" --yes-label "Sí" --no-label "No" --yesno "$1"
    elif _hay zenity; then zenity --question --title="DeckLink" --width=460 --ok-label="Sí" --cancel-label="No" --text="$1"
    else return 0; fi
}

dlg_pass() {
    if _hay kdialog; then kdialog --title "DeckLink" --icon "$ICONO" --password "$1"
    elif _hay zenity; then zenity --password --title="$1"
    else return 1; fi
}

# dlg_menu "texto" clave1 "etiqueta1" clave2 "etiqueta2" ...  -> imprime la clave elegida
dlg_menu() {
    local texto="$1"; shift
    if _hay kdialog; then
        kdialog --title "DeckLink" --icon "$ICONO" --menu "$texto" "$@"
    elif _hay zenity; then
        local args=() k
        while [ $# -gt 1 ]; do args+=("$1" "$2"); shift 2; done
        zenity --list --title="DeckLink" --width=480 --height=360 --text="$texto" \
               --column="id" --column="Opción" --hide-column=1 --print-column=1 "${args[@]}"
    else
        return 1
    fi
}

# Carpeta del escritorio del usuario (Escritorio, Desktop...)
carpeta_escritorio() {
    local d=""
    _hay xdg-user-dir && d=$(xdg-user-dir DESKTOP 2>/dev/null)
    [ -n "$d" ] && [ "$d" != "$HOME" ] || d="$HOME/Desktop"
    [ -d "$d" ] || { [ -d "$HOME/Escritorio" ] && d="$HOME/Escritorio"; }
    mkdir -p "$d"
    echo "$d"
}

# Pone un lanzador en el escritorio y lo marca como de confianza
poner_en_escritorio() {
    local origen="$1" nombre="$2" esc
    esc=$(carpeta_escritorio)
    cp -f "$origen" "$esc/$nombre"
    chmod 755 "$esc/$nombre"
    _hay gio && gio set "$esc/$nombre" metadata::trusted true 2>/dev/null
    true
}

quitar_de_escritorio() {
    local esc
    esc=$(carpeta_escritorio)
    rm -f "$esc/$1"
}
