#!/usr/bin/env bash
# =============================================================================
#  macos/install.sh — instala "GB Recompiled.app" (se ejecuta UNA vez)
#
#  1. Comprueba Xcode Command Line Tools y Homebrew.
#  2. Instala con Homebrew solo lo que falte de cmake, ninja y sdl2.
#  3. Compila el recompilador (gbrecomp) y el launcher gráfico.
#  4. Crea "GB Recompiled.app" en ~/Applications (con icono) y la abre.
#
#  Después, todo se hace desde la app: añadir ROM -> Recompilar -> Jugar.
#  Para actualizar:  git pull && ./macos/install.sh
#
#  Variables opcionales:
#    APP_DIR=/ruta     carpeta donde crear la app (por defecto ~/Applications)
#    NO_OPEN=1         no abrir la app al terminar
#  Compatible con el bash 3.2 de macOS.
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP_DIR="${APP_DIR:-$HOME/Applications}"
APP="$APP_DIR/GB Recompiled.app"
BUILD="$ROOT/build-launcher"

info() { printf '\n\033[1;32m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m[aviso] %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31m[error] %s\033[0m\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
info "1/4  Comprobando herramientas del sistema"
if [ "$(uname -s)" = "Darwin" ]; then
    if ! xcode-select -p >/dev/null 2>&1; then
        warn "Faltan las Xcode Command Line Tools. Se abrirá el instalador;"
        warn "cuando termine, vuelve a ejecutar ./macos/install.sh"
        xcode-select --install || true
        exit 1
    fi
    echo "    Xcode Command Line Tools: OK"
    command -v brew >/dev/null 2>&1 || die "No se encontró Homebrew. Instálalo desde https://brew.sh y repite."
    echo "    Homebrew: OK ($(brew --prefix))"

    info "2/4  Dependencias (cmake, ninja, sdl2): se instala solo lo que falte"
    MISSING=""
    command -v cmake >/dev/null 2>&1 || MISSING="$MISSING cmake"
    command -v ninja >/dev/null 2>&1 || MISSING="$MISSING ninja"
    if ! brew list --versions sdl2 >/dev/null 2>&1 && ! command -v sdl2-config >/dev/null 2>&1; then
        MISSING="$MISSING sdl2"
    fi
    if [ -n "$MISSING" ]; then
        echo "    Por instalar:$MISSING"
        echo "    (en Mac Intel Homebrew compila desde código fuente: puede tardar 15-40 min; no cierres la Terminal)"
        # shellcheck disable=SC2086
        brew install $MISSING
    else
        echo "    Ya está todo instalado."
    fi
else
    warn "No estás en macOS ($(uname -s)): se omite Homebrew y se crea el paquete igualmente (para pruebas)."
    info "2/4  (omitido fuera de macOS)"
fi
for tool in cmake ninja; do
    command -v "$tool" >/dev/null 2>&1 || die "Falta '$tool' en el PATH."
done

# ---------------------------------------------------------------------------
info "3/4  Compilando el recompilador y el launcher"
cmake -G Ninja -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DGBRECOMP_BUILD_LAUNCHER=ON
ninja -C "$BUILD" gbrecomp gbrecomp-launcher

# ---------------------------------------------------------------------------
info "4/4  Creando \"GB Recompiled.app\" en $APP_DIR"
mkdir -p "$APP_DIR"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BUILD/bin/gbrecomp-launcher" "$APP/Contents/MacOS/GB Recompiled"
cp "$BUILD/bin/gbrecomp" "$APP/Contents/Resources/gbrecomp"
# Runtime que gbrecomp copia dentro de cada juego generado (+ su licencia).
mkdir -p "$APP/Contents/Resources/runtime"
(cd "$ROOT/runtime" && tar cf - --exclude='./build*' .) | (cd "$APP/Contents/Resources/runtime" && tar xf -)
cp "$ROOT/LICENSE" "$APP/Contents/Resources/LICENSE"

VERSION="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo dev)"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>               <string>GB Recompiled</string>
    <key>CFBundleDisplayName</key>        <string>GB Recompiled</string>
    <key>CFBundleIdentifier</key>         <string>io.github.gabom88.gb-recompiled</string>
    <key>CFBundleExecutable</key>         <string>GB Recompiled</string>
    <key>CFBundleIconFile</key>           <string>AppIcon</string>
    <key>CFBundlePackageType</key>        <string>APPL</string>
    <key>CFBundleShortVersionString</key> <string>0.1.0</string>
    <key>CFBundleVersion</key>            <string>${VERSION}</string>
    <key>LSMinimumSystemVersion</key>     <string>10.15</string>
    <key>NSHighResolutionCapable</key>    <true/>
    <key>LSApplicationCategoryType</key>  <string>public.app-category.games</string>
</dict>
</plist>
PLIST

# Icono (.icns) a partir del PNG, con las herramientas de macOS.
if command -v sips >/dev/null 2>&1 && command -v iconutil >/dev/null 2>&1; then
    ICONSET="$BUILD/AppIcon.iconset"
    rm -rf "$ICONSET"; mkdir -p "$ICONSET"
    for s in 16 32 128 256 512; do
        sips -z $s $s "$ROOT/launcher/assets/AppIcon.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
        d=$((s * 2))
        sips -z $d $d "$ROOT/launcher/assets/AppIcon.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
    done
    iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns" || warn "No se pudo crear el icono (no es grave)."
else
    cp "$ROOT/launcher/assets/AppIcon.png" "$APP/Contents/Resources/AppIcon.png"
fi

# Firma local "ad-hoc" (sin cuenta de desarrollador): evita avisos al abrirla.
if command -v codesign >/dev/null 2>&1; then
    codesign --force --deep -s - "$APP" >/dev/null 2>&1 || warn "codesign falló (no es grave en un Mac Intel)."
fi

info "Listo: $APP"
echo "    Ábrela desde Finder > Aplicaciones (de tu usuario), Launchpad o Spotlight: \"GB Recompiled\"."
echo "    Tus juegos y partidas se guardan en: ~/Library/Application Support/GB Recompiled"
if [ "$(uname -s)" = "Darwin" ] && [ "${NO_OPEN:-0}" != "1" ]; then
    open "$APP"
fi
