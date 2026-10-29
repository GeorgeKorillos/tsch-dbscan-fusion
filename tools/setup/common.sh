#!/usr/bin/env bash
# Shared installation steps for the two supported Linux hosts.
# This file is sourced by the entry points, not executed directly.
source "$SETUP_DIR/versions/versions.sh"

fail() { printf 'Setup: %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'HELP'
Usage: bash tools/setup/setup-{ubuntu,raspberry-pi}.sh [options]
  --stage NAME       all (default), packages, downloads, toolchain,
                     contiki, java, cooja, or environment
  --prefix DIR       private tools directory (default: ~/opt/thesis-tools)
  --work-dir DIR     downloads and compiler builds (default: ~/.cache/thesis-setup)
  --contiki-dir DIR  OS checkout (default: contiki-ng beside this project)
  --jobs N           build parallelism (default: 1)
  --help             show this text without changing anything
No firmware, trace header or simulation is built or run by this installer.
HELP
}

as_root() {
  if (( EUID == 0 )); then "$@"; else sudo "$@"; fi
}

install_packages() {
  as_root apt-get update
  local sound=libasound2
  if apt-cache show libasound2t64 >/dev/null 2>&1; then sound=libasound2t64; fi
  as_root apt-get install -y build-essential git curl ca-certificates \
    bzip2 xz-utils unzip patch flex bison libncurses-dev zlib1g-dev \
    libtool automake autoconf python3 python3-serial python3-matplotlib \
    srecord file libx11-6 libxext6 libxi6 libxrender1 libxtst6 libfreetype6 \
    fontconfig "$sound"
  if [[ "$PLATFORM" == ubuntu ]]; then
    # The historical prebuilt MSP430 compiler is an i386 executable.
    as_root apt-get install -y libc6-i386 lib32stdc++6 lib32z1
    if apt-cache show lib32ncurses6 >/dev/null 2>&1; then
      as_root apt-get install -y lib32ncurses6 lib32tinfo6
    fi
    if apt-cache show lib32ncurses5 >/dev/null 2>&1; then
      as_root apt-get install -y lib32ncurses5 lib32tinfo5
    fi
  fi
}

fetch() {
  local name="$1" sha url extra target
  read -r sha url extra <<< "$(awk -v n="$name" '$1 == n {print $2, $3}' \
    "$SETUP_DIR/versions/downloads.tsv")"
  [[ "$sha" =~ ^[[:xdigit:]]{64}$ && -n "$url" ]] || fail "No pinned download: $name"
  target="$CACHE/$name"
  if [[ ! -f "$target" ]]; then
    curl --fail --location --retry 3 --connect-timeout 30 \
      "$url" -o "$target.part"
    mv -- "$target.part" "$target"
  fi
  printf '%s  %s\n' "$sha" "$target" | sha256sum --check --status || \
    fail "Checksum mismatch: $target. Remove that cached file and retry."
}

download_sources() {
  local name
  if [[ "$PLATFORM" == ubuntu ]]; then
    fetch mspgcc-4.7.2-compiled.tar.bz2
  else
    for name in gmp-4.3.2.tar.bz2 mpfr-2.4.2.tar.bz2 mpc-0.8.1.tar.gz \
      binutils-2.22.tar.bz2 gcc-4.7.0.tar.bz2 mspgcc-20120911.tar.bz2 \
      gcc-4.7.0-patches.tar.xz msp430mcu-20130321.tar.bz2 \
      msp430-libc-20120716.tar.bz2 config.guess config.sub; do
      fetch "$name"
    done
  fi
  fetch "$JDK_ARCHIVE"
}

apply_once() {
  local tree="$1" change="$2"
  if patch --directory="$tree" --strip=1 --forward --dry-run < "$change" >/dev/null 2>&1; then
    patch --directory="$tree" --strip=1 --forward < "$change"
  elif patch --directory="$tree" --strip=1 --reverse --dry-run < "$change" >/dev/null 2>&1; then
    : # The same patch was already applied during an earlier attempt.
  else
    fail "Patch does not match the source: $change"
  fi
}

unpack() {
  local archive="$1" directory="$2"
  fetch "$archive"
  if [[ ! -f "$directory/.unpacked" ]]; then
    [[ ! -e "$directory" ]] || fail "Incomplete extraction: $directory; move it aside and retry."
    mkdir -p "$directory"
    tar --no-same-owner -xf "$CACHE/$archive" --strip-components=1 -C "$directory"
    touch "$directory/.unpacked"
  fi
}

update_config_scripts() {
  local tree="$1"
  find "$tree" -type f \( -name config.guess -o -name configfsf.guess \) \
    -exec cp "$CACHE/config.guess" {} \;
  find "$tree" -type f -name config.sub -exec cp "$CACHE/config.sub" {} \;
}

build_library() {
  local name="$1"; shift
  [[ ! -f "$BUILD/$name.done" ]] || return 0
  mkdir -p "$BUILD/$name"
  (cd "$BUILD/$name"
    "$SOURCES/$name/configure" --build=aarch64-unknown-linux-gnu \
      --host=aarch64-unknown-linux-gnu --prefix="$PREREQS" --disable-shared "$@"
    make -j"$JOBS"
    make install)
  touch "$BUILD/$name.done"
}

install_toolchain() {
  if [[ -f "$TOOLCHAIN/.thesis-setup-complete" ]]; then return 0; fi
  if [[ "$PLATFORM" == ubuntu ]]; then
    unpack mspgcc-4.7.2-compiled.tar.bz2 "$TOOLCHAIN"
  else
    (build_pi_toolchain)
  fi
  # This stamp marks installation only; it does not certify a firmware build.
  touch "$TOOLCHAIN/.thesis-setup-complete"
}

build_pi_toolchain() {
  local name available
  available=$(df -Pk "$WORK_DIR" | awk 'NR == 2 {print $4}')
  (( available >= 6 * 1024 * 1024 )) || fail "At least 6 GiB of free build space is required. Use --work-dir on a larger disk."
  for name in gmp-4.3.2 mpfr-2.4.2 binutils-2.22 gcc-4.7.0 \
    mspgcc-20120911 msp430mcu-20130321 msp430-libc-20120716; do
    unpack "$name.tar.bz2" "$SOURCES/$name"
  done
  unpack mpc-0.8.1.tar.gz "$SOURCES/mpc-0.8.1"
  fetch config.guess; fetch config.sub
  for name in gmp-4.3.2 mpfr-2.4.2 mpc-0.8.1 binutils-2.22 gcc-4.7.0; do
    update_config_scripts "$SOURCES/$name"
  done
  apply_once "$SOURCES/binutils-2.22" "$SOURCES/mspgcc-20120911/msp430-binutils-2.22-20120911.patch"
  apply_once "$SOURCES/gcc-4.7.0" "$SOURCES/mspgcc-20120911/msp430-gcc-4.7.0-20120911.patch"
  fetch gcc-4.7.0-patches.tar.xz
  mkdir -p "$SOURCES/gcc-extra-patches"
  tar --no-same-owner -xf "$CACHE/gcc-4.7.0-patches.tar.xz" -C "$SOURCES/gcc-extra-patches"
  for name in PR-54638 gperf libiberty-multilib; do
    apply_once "$SOURCES/gcc-4.7.0" "$SOURCES/gcc-extra-patches/gcc-4.7.0_$name.patch"
  done
  apply_once "$SOURCES/gcc-4.7.0" "$SETUP_DIR/patches/msp430-value-index.patch"

  # Old configure probes and C sources predate GCC 14's stricter defaults.
  # Keep these flags scoped to this build, rather than changing the host compiler.
  export CC=gcc CXX=g++
  export CFLAGS='-O2 -std=gnu89 -Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=return-mismatch'
  export CXXFLAGS='-O2 -std=gnu++98'
  export CC_FOR_BUILD="gcc $CFLAGS"
  # Tool documentation is not needed for Cooja. Avoid rebuilding old Texinfo.
  export MAKEINFO=true
  build_library gmp-4.3.2
  build_library mpfr-2.4.2 --with-gmp="$PREREQS"
  build_library mpc-0.8.1 --with-gmp="$PREREQS" --with-mpfr="$PREREQS"
  mkdir -p "$TOOLCHAIN" "$BUILD/binutils" "$BUILD/gcc"
  export PATH="$TOOLCHAIN/bin:$PATH"
  if [[ ! -f "$BUILD/binutils.done" ]]; then
    (cd "$BUILD/binutils"
      "$SOURCES/binutils-2.22/configure" --target=msp430 --program-prefix=msp430- \
        --prefix="$TOOLCHAIN" --build=aarch64-unknown-linux-gnu \
        --host=aarch64-unknown-linux-gnu --disable-nls --disable-werror
      make -j"$JOBS" MAKEINFO=true
      make MAKEINFO=true install)
    touch "$BUILD/binutils.done"
  fi
  if [[ ! -f "$BUILD/gcc.done" ]]; then
    (cd "$BUILD/gcc"
      "$SOURCES/gcc-4.7.0/configure" --target=msp430 --enable-languages=c \
        --program-prefix=msp430- --prefix="$TOOLCHAIN" \
        --build=aarch64-unknown-linux-gnu --host=aarch64-unknown-linux-gnu \
        --disable-nls --disable-werror --disable-shared \
        --with-gmp="$PREREQS" --with-mpfr="$PREREQS" --with-mpc="$PREREQS"
      make -j"$JOBS" MAKEINFO=true
      make MAKEINFO=true install)
    touch "$BUILD/gcc.done"
  fi
  (cd "$SOURCES/msp430mcu-20130321"
    export MSP430MCU_ROOT="$PWD"
    bash scripts/install.sh "$TOOLCHAIN/")
  (cd "$SOURCES/msp430-libc-20120716/src"
    # Firmware library rules choose their own target flags.
    unset CFLAGS CXXFLAGS CC CXX CC_FOR_BUILD
    make -j"$JOBS"
    make PREFIX="$TOOLCHAIN" install)
}

install_java() {
  unpack "$JDK_ARCHIVE" "$JAVA_HOME"
}

install_contiki() {
  if [[ ! -e "$CONTIKI_DIR" ]]; then
    git clone https://github.com/contiki-ng/contiki-ng.git "$CONTIKI_DIR"
    git -C "$CONTIKI_DIR" checkout --detach "$CONTIKI_COMMIT"
  else
    [[ -d "$CONTIKI_DIR/.git" ]] || fail "Not a Git checkout: $CONTIKI_DIR"
    [[ $(git -C "$CONTIKI_DIR" rev-parse HEAD) == "$CONTIKI_COMMIT" ]] || \
      fail "Existing Contiki revision differs. Choose a new --contiki-dir; no checkout was overwritten."
  fi
  # Z1 simulation needs Cooja and its own dependencies, not unrelated MCU SDKs.
  git -C "$CONTIKI_DIR" submodule update --init --recursive -- tools/cooja
  [[ $(git -C "$CONTIKI_DIR/tools/cooja" rev-parse HEAD) == "$COOJA_COMMIT" ]] || \
    fail "Unexpected Cooja revision."
  apply_once "$CONTIKI_DIR" "$SETUP_DIR/patches/contiki-neighbor-initializer.patch"
}

build_cooja() {
  [[ -x "$JAVA_HOME/bin/java" ]] || fail "Install the java stage first."
  [[ -f "$CONTIKI_DIR/tools/cooja/gradlew" ]] || fail "Install the contiki stage first."
  (cd "$CONTIKI_DIR/tools/cooja"
    export JAVA_HOME PATH="$JAVA_HOME/bin:$PATH"
    bash ./gradlew --no-daemon --max-workers="$JOBS" \
      -Dorg.gradle.jvmargs=-Xmx512m assemble)
}

write_environment() {
  # Bash quoting keeps paths with shell metacharacters literal in the saved file.
  local activate="$PREFIX/activate.sh" line
  {
    printf '# Development tools for the thesis project.\n'
    printf 'export CONTIKI=%q\n' "$CONTIKI_DIR"
    printf 'export JAVA_HOME=%q\n' "$JAVA_HOME"
    printf 'export PATH=%q:%q:"$PATH"\n' "$TOOLCHAIN/bin" "$JAVA_HOME/bin"
  } > "$activate"
  printf -v line 'source %q' "$activate"
  touch "$HOME/.bashrc"
  grep -Fxq "$line" "$HOME/.bashrc" || printf '\n%s\n' "$line" >> "$HOME/.bashrc"
}

setup_main() {
  PLATFORM="$1"; shift
  PROJECT_DIR=$(cd "$SETUP_DIR/../.." && pwd)
  PREFIX="$HOME/opt/thesis-tools"
  WORK_DIR="$HOME/.cache/thesis-setup"
  CONTIKI_DIR="$(dirname "$PROJECT_DIR")/contiki-ng"
  JOBS=1; STAGE=all
  while (( $# )); do
    case "$1" in
      --help|-h) usage; return 0 ;;
      --prefix|--work-dir|--contiki-dir|--jobs|--stage)
        (( $# >= 2 )) || fail "Missing value for $1"
        case "$1" in
          --prefix) PREFIX="$2";; --work-dir) WORK_DIR="$2";;
          --contiki-dir) CONTIKI_DIR="$2";; --jobs) JOBS="$2";; --stage) STAGE="$2";;
        esac; shift 2 ;;
      *) fail "Unknown option: $1" ;;
    esac
  done
  [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail "--jobs must be a positive integer"
  case "$STAGE" in all|packages|downloads|toolchain|contiki|java|cooja|environment) ;;
    *) fail "Unknown stage: $STAGE";; esac
  [[ $(uname -s) == Linux ]] || fail "Linux is required"
  source /etc/os-release
  if [[ "$PLATFORM" == ubuntu ]]; then
    [[ "$ID" == ubuntu && $(uname -m) == x86_64 ]] || fail "This entry point requires Ubuntu x86-64."
    ARCH=x64; TOOLCHAIN_VERSION=4.7.2
  else
    [[ ( "$ID" == debian || "$ID" == raspbian ) && $(uname -m) == aarch64 ]] || \
      fail "This entry point requires Debian/Raspberry Pi OS on AArch64."
    [[ "$VERSION_ID" == 13 ]] || fail "The Pi recipe targets Debian/Raspberry Pi OS 13 (GCC 14)."
    ARCH=aarch64; TOOLCHAIN_VERSION=4.7.0
  fi
  PREFIX=$(realpath -m "$PREFIX"); WORK_DIR=$(realpath -m "$WORK_DIR")
  CONTIKI_DIR=$(realpath -m "$CONTIKI_DIR")
  # These old makefiles do not reliably support whitespace in toolchain paths.
  [[ "$PREFIX$WORK_DIR$CONTIKI_DIR" != *[[:space:]]* ]] || fail "Use paths without whitespace."
  TOOLCHAIN="$PREFIX/msp430-$TOOLCHAIN_VERSION"
  PREREQS="$PREFIX/msp430-prerequisites"
  JAVA_HOME="$PREFIX/jdk-21.0.8-$ARCH"
  JDK_ARCHIVE="OpenJDK21U-jdk_${ARCH}_linux_hotspot_21.0.8_9.tar.gz"
  CACHE="$WORK_DIR/downloads"; SOURCES="$WORK_DIR/sources-$ARCH"; BUILD="$WORK_DIR/build-$ARCH"
  mkdir -p "$PREFIX" "$CACHE" "$SOURCES" "$BUILD"
  if [[ "$PLATFORM" == raspberry-pi && ( "$STAGE" == all || "$STAGE" == toolchain ) && ! -f "$TOOLCHAIN/.thesis-setup-complete" ]]; then
    local free_kib
    free_kib=$(df -Pk "$WORK_DIR" | awk 'NR == 2 {print $4}')
    (( free_kib >= 6 * 1024 * 1024 )) || fail "At least 6 GiB free is required. Select a larger --work-dir."
  fi
  # Refuse to reuse compiler objects with a different destination or recipe.
  local recipe saved="$BUILD/recipe.txt"
  recipe=$( { printf '%s\n' "$PREFIX" "$PLATFORM"; cat "$SETUP_DIR/common.sh" \
    "$SETUP_DIR/versions/versions.sh" "$SETUP_DIR/versions/downloads.tsv" \
    "$SETUP_DIR/patches/"*.patch; } | sha256sum | cut -d' ' -f1)
  if [[ -f "$saved" && $(cat "$saved") != "$recipe" ]]; then
    fail "Build recipe changed. Select a fresh --work-dir and --prefix."
  fi
  printf '%s\n' "$recipe" > "$saved"
  case "$STAGE" in
    all) install_packages; download_sources; install_toolchain; install_java;
         install_contiki; build_cooja; write_environment ;;
    packages) install_packages;; downloads) download_sources;; toolchain) install_toolchain;;
    java) install_java;; contiki) install_contiki;; cooja) build_cooja;; environment) write_environment;;
  esac
}
