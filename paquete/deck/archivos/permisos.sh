# permisos.sh — consigue permiso de administrador en la Steam Deck con ventanas.
# Define la función root() para ejecutar órdenes como administrador.

SUDO_PASS=""

_crear_contrasena() {
    dlg_info "Tu Steam Deck todavía no tiene contraseña de administrador y DeckLink la necesita para instalarse.

Ahora te pediremos una nueva. Elige una que recuerdes (mejor de 6 letras o más)."
    local p1 p2
    for _ in 1 2 3; do
        p1=$(dlg_pass "Nueva contraseña para la Steam Deck:") || return 1
        p2=$(dlg_pass "Repite la nueva contraseña:") || return 1
        if [ -z "$p1" ] || [ "$p1" != "$p2" ]; then
            dlg_error "Las contraseñas están vacías o no coinciden. Prueba otra vez."
            continue
        fi
        if printf '%s\n%s\n' "$p1" "$p1" | passwd >/dev/null 2>&1; then
            SUDO_PASS="$p1"
            return 0
        fi
        dlg_error "No se ha podido poner esa contraseña (quizá es demasiado corta o sencilla). Prueba con otra."
    done
    return 1
}

obtener_root() {
    if sudo -n true 2>/dev/null; then
        root() { sudo "$@"; }
        return 0
    fi
    local estado
    estado=$(passwd -S "$USER" 2>/dev/null | awk '{print $2}')
    if [ "$estado" = "NP" ]; then
        _crear_contrasena || return 1
    fi
    for _ in 1 2 3; do
        if [ -z "$SUDO_PASS" ]; then
            SUDO_PASS=$(dlg_pass "Escribe la contraseña de la Steam Deck (la de administrador).

Si nunca has puesto ninguna, déjalo vacío y pulsa Aceptar.") || return 1
            if [ -z "$SUDO_PASS" ]; then
                _crear_contrasena || return 1
            fi
        fi
        if printf '%s\n' "$SUDO_PASS" | sudo -S -p '' -v 2>/dev/null; then
            root() { printf '%s\n' "$SUDO_PASS" | sudo -S -p '' "$@"; }
            return 0
        fi
        SUDO_PASS=""
        dlg_error "Contraseña incorrecta."
    done
    return 1
}
