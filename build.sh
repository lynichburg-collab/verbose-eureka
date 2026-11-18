#!/usr/bin/env bash
# build.sh — builds the whole OS from source.
#
#   ./build.sh            build everything and produce out/kernel + out/rootfs.cpio.gz
#   ./build.sh <package>  (re)build one package, e.g. ./build.sh busybox
#   ./build.sh image      reassemble the root filesystem and initramfs only
#   ./build.sh test       boot the result in QEMU and run the self-test
#   ./build.sh run        boot the result in QEMU interactively (Ctrl-A X quits)
#   ./build.sh clean      delete build output, keep downloaded sources
#   ./build.sh distclean  delete everything, including downloaded sources
#
# Each package is described by recipes/<name>.recipe (see recipes/README.md).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=os.conf
. "$ROOT/os.conf"

WORK="${WORK:-$ROOT/work}"
SOURCES="$WORK/sources"
BUILDS="$WORK/build"
ROOTFS="$WORK/rootfs"
SYSROOT="$WORK/sysroot"          # kernel headers for userland builds
OUT="${OUT:-$ROOT/out}"
JOBS="${JOBS:-$(nproc)}"
export ROOT WORK SOURCES BUILDS ROOTFS SYSROOT OUT JOBS ARCH OS_ID OS_NAME OS_VERSION

# Userland is linked statically against musl. Kernel headers come from our own
# kernel source (installed into $SYSROOT by the linux recipe), not the host.
export TARGET_CC="musl-gcc"
export TARGET_CFLAGS="-Os -pipe -static -isystem $SYSROOT/include"
export TARGET_LDFLAGS="-static"

log()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

need_tools() {
    local missing=()
    for t in git make gcc musl-gcc flex bison bc cpio gzip rsync depmod sha256sum; do
        command -v "$t" >/dev/null 2>&1 || missing+=("$t")
    done
    [ ${#missing[@]} -eq 0 ] || die "missing host tools: ${missing[*]} (see README: Building)"
}

# --- recipes ----------------------------------------------------------------

recipe_file() { echo "$ROOT/recipes/$1.recipe"; }

# Load a recipe into a clean set of variables. Call inside a subshell.
load_recipe() {
    local name="$1" file
    file="$(recipe_file "$name")"
    [ -f "$file" ] || die "no recipe for '$name' ($file)"
    NAME="" VERSION="" SOURCE_GIT="" SOURCE_REF="" SOURCE_LOCAL="" INPUTS=""
    unset -f build install 2>/dev/null || true
    # shellcheck source=/dev/null
    . "$file"
    [ "$NAME" = "$name" ] || die "$file: NAME must be '$name'"
    if [ -n "$SOURCE_LOCAL" ]; then
        SRC="$ROOT/$SOURCE_LOCAL"
    else
        SRC="$SOURCES/$NAME-$VERSION"
    fi
    BUILD="$BUILDS/$NAME"
    export NAME VERSION SRC BUILD
}

fetch() {
    [ -n "$SOURCE_LOCAL" ] && return 0
    if [ -d "$SRC/.git" ] || [ -f "$SRC/.fetched" ]; then return 0; fi
    log "fetch $NAME $VERSION ($SOURCE_GIT @ $SOURCE_REF)"
    rm -rf "$SRC.tmp"
    git clone --quiet --depth 1 --branch "$SOURCE_REF" "$SOURCE_GIT" "$SRC.tmp"
    mv "$SRC.tmp" "$SRC"
}

# A package is rebuilt when its recipe or any of its INPUTS files change.
fingerprint() {
    {
        cat "$(recipe_file "$NAME")"
        for f in $INPUTS; do
            if [ -d "$ROOT/$f" ]; then
                find "$ROOT/$f" -type f -print0 | sort -z | xargs -0 cat
            else
                cat "$ROOT/$f"
            fi
        done
        echo "$TARGET_CC $TARGET_CFLAGS"
    } | sha256sum | cut -d' ' -f1
}

build_pkg() (
    load_recipe "$1"
    fetch
    local fp stamp="$BUILD/.built"
    fp="$(fingerprint)"
    if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$fp" ] && [ "${FORCE:-0}" != 1 ]; then
        log "$NAME $VERSION is up to date"
        return 0
    fi
    log "build $NAME $VERSION"
    mkdir -p "$BUILD"
    rm -f "$stamp"
    build
    echo "$fp" > "$stamp"
)

install_pkg() (
    load_recipe "$1"
    [ -f "$BUILD/.built" ] || die "$NAME has not been built"
    install
)

# --- root filesystem and image ----------------------------------------------

assemble_rootfs() {
    log "assemble root filesystem"
    rm -rf "$ROOTFS"
    mkdir -p "$ROOTFS"
    # Static files that ship with the OS (config, boot scripts, motd...).
    cp -a "$ROOT/rootfs/." "$ROOTFS/"
    # Our filesystem layout. See README: Filesystem layout.
    mkdir -p "$ROOTFS"/{system/bin,system/lib,system/share,system/modules,system/firmware} \
             "$ROOTFS"/{config,home/root,var/log,dev,proc,sys,run,tmp}
    chmod 1777 "$ROOTFS/tmp"
    chmod 0700 "$ROOTFS/home/root"
    # Compatibility link: musl and a few BusyBox applets look in /etc. This
    # goes away once we patch those paths at build time.
    ln -sfn config "$ROOTFS/etc"
    # /config/resolv.conf lives in /run so /config can later be read-only.
    ln -sfn /run/resolv.conf "$ROOTFS/config/resolv.conf"

    cat > "$ROOTFS/config/os-release" <<EOF
ID=$OS_ID
NAME="$OS_NAME"
VERSION="$OS_VERSION"
PRETTY_NAME="$OS_NAME $OS_VERSION"
EOF
    echo "$OS_ID" > "$ROOTFS/config/hostname"

    for p in $PACKAGES; do install_pkg "$p"; done
}

# Build the initramfs with the kernel's own gen_init_cpio, so device nodes and
# ownership are correct without needing root on the build machine.
make_image() {
    local gen="$BUILDS/linux/usr/gen_init_cpio" list="$WORK/initramfs.list"
    [ -x "$gen" ] || die "gen_init_cpio not found; build linux first"
    log "create initramfs"
    {
        echo "nod /dev/console 0600 0 0 c 5 1"
        echo "nod /dev/null 0666 0 0 c 1 3"
        (cd "$ROOTFS" && find . -mindepth 1 | LC_ALL=C sort) | while read -r p; do
            local rel="${p#./}" full="$ROOTFS/${p#./}" mode
            [ "$rel" = "dev/console" ] || [ "$rel" = "dev/null" ] && continue
            mode="$(stat -c '%a' "$full")"
            if [ -L "$full" ]; then
                echo "slink /$rel $(readlink "$full") 0777 0 0"
            elif [ -d "$full" ]; then
                echo "dir /$rel $mode 0 0"
            else
                echo "file /$rel $full $mode 0 0"
            fi
        done
    } > "$list"
    mkdir -p "$OUT"
    "$gen" "$list" | gzip -9 > "$OUT/rootfs.cpio.gz"
    log "out/kernel ($(du -h "$OUT/kernel" | cut -f1)), out/rootfs.cpio.gz ($(du -h "$OUT/rootfs.cpio.gz" | cut -f1))"
}

# --- QEMU -------------------------------------------------------------------

qemu_cmd() {
    local accel="tcg"
    [ -w /dev/kvm ] && accel="kvm"
    echo qemu-system-x86_64 -accel "$accel" -m 1024 -smp 2 -nographic -no-reboot \
        -kernel "$OUT/kernel" -initrd "$OUT/rootfs.cpio.gz" \
        -netdev user,id=n0 -device virtio-net-pci,netdev=n0
}

cmd_run() {
    log "booting in QEMU — Ctrl-A then X to quit"
    # shellcheck disable=SC2046
    exec $(qemu_cmd) -append "console=ttyS0 rdinit=/system/bin/init"
}

cmd_test() {
    local log_file="$OUT/boot-test.log"
    log "boot test (log: $log_file)"
    # shellcheck disable=SC2046
    timeout 600 $(qemu_cmd) -append "console=ttyS0 rdinit=/system/bin/init ve.test=1 panic=-1" \
        < /dev/null > "$log_file" 2>&1 || true
    if grep -q "SELFTEST PASS" "$log_file" && grep -q "reboot: Power down" "$log_file"; then
        grep -E "SELFTEST|^  (ok|FAIL)" "$log_file" || true
        log "boot test passed"
    else
        tail -n 60 "$log_file"
        die "boot test failed"
    fi
}

# --- main -------------------------------------------------------------------

main() {
    local cmd="${1:-all}"
    case "$cmd" in
        all)
            need_tools
            for p in $PACKAGES; do build_pkg "$p"; done
            assemble_rootfs
            make_image
            ;;
        image)     need_tools; assemble_rootfs; make_image ;;
        test)      cmd_test ;;
        run)       cmd_run ;;
        clean)     rm -rf "$BUILDS" "$ROOTFS" "$SYSROOT" "$OUT" "$WORK/initramfs.list" ;;
        distclean) rm -rf "$WORK" "$OUT" ;;
        *)
            [ -f "$(recipe_file "$cmd")" ] || die "unknown command or package: $cmd"
            need_tools
            FORCE=1 build_pkg "$cmd"
            ;;
    esac
}

main "$@"
