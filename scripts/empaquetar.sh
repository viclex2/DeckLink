#!/bin/bash
# Crea los dos ZIP de instalación en ./dist a partir de build/ y paquete/.
set -e
cd "$(dirname "$0")/.."
[ -x build/decklink-deck ] && [ -x build/decklink-portatil ] || { echo "Primero ejecuta scripts/compilar.sh"; exit 1; }
rm -rf dist && mkdir -p dist/DeckLink-SteamDeck/archivos dist/DeckLink-Portatil/archivos

cp "paquete/deck/Instalar DeckLink" paquete/deck/LEEME.txt dist/DeckLink-SteamDeck/
cp build/decklink-deck paquete/deck/archivos/* paquete/comun/dialogos.sh \
   arte/decklink.svg arte/decklink-256.png dist/DeckLink-SteamDeck/archivos/

cp "paquete/portatil/Instalar DeckLink" paquete/portatil/LEEME.txt dist/DeckLink-Portatil/
cp build/decklink-portatil paquete/portatil/archivos/* paquete/comun/dialogos.sh \
   arte/decklink.svg arte/decklink-256.png dist/DeckLink-Portatil/archivos/

find dist -type f -exec chmod 644 {} +
chmod 755 dist/*/"Instalar DeckLink" dist/DeckLink-SteamDeck/archivos/{decklink-deck,decklink-panel,desinstalar.sh} \
          dist/DeckLink-Portatil/archivos/{decklink-portatil,desinstalar.sh}

# ZIP que conserva los permisos de ejecución (para que baste con doble clic)
python3 - <<'EOF'
import zipfile, os, stat
def hacer(carpeta, salida):
    with zipfile.ZipFile(salida, "w", zipfile.ZIP_DEFLATED) as z:
        base = os.path.dirname(carpeta)
        for raiz, dirs, archivos in os.walk(carpeta):
            dirs.sort()
            zi = zipfile.ZipInfo(os.path.relpath(raiz, base) + "/")
            zi.external_attr = (0o40755 << 16) | 0x10; zi.create_system = 3
            z.writestr(zi, b"")
            for a in sorted(archivos):
                ruta = os.path.join(raiz, a)
                zi = zipfile.ZipInfo(os.path.relpath(ruta, base))
                zi.external_attr = (stat.S_IFREG | (os.stat(ruta).st_mode & 0o777)) << 16
                zi.create_system = 3; zi.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(zi, open(ruta, "rb").read())
hacer("dist/DeckLink-SteamDeck", "dist/DeckLink-SteamDeck.zip")
hacer("dist/DeckLink-Portatil", "dist/DeckLink-Portatil.zip")
EOF
echo "Listos: dist/DeckLink-SteamDeck.zip y dist/DeckLink-Portatil.zip"
