# Hierarchical Data Fusion with DBSCAN over TSCH: Design and Evaluation in Contiki-NG

Application-layer forwarding and distributed fusion on Contiki-NG, without RPL.

This project runs a sensor, relay and collector application on Contiki-NG Z1
motes. Sensors choose a nearby parent, relays group measurements with DBSCAN (MinPts=1) and
exchange contributions with same-level peers, and the master prints a report
once every five seconds. All roles use IPv6 and TSCH with nullrouting. A relay
receives a packet addressed to itself and sends a new application packet to its
parent; the IPv6 stack does not route that original packet across the tree.

The recommended starting point is `shared-adaptive` with length `3`. A length
of `5` reduces radio duty cycle but has produced more incomplete aggregates.
The receiver-based profiles are retained as experimental comparisons, not as
replacements with equivalent reliability.

## Install the development environment

The application targets simulated Z1/MSP430 motes. The computer running the
compiler and Cooja can be an Ubuntu x86-64 machine or a Raspberry Pi running a
64-bit Debian-based system. Raspberry Pi is the development host, not the mote
platform.

The development environments are Ubuntu 20.04.6 under WSL with MSP430 GCC
4.7.2, and Debian 13.1 on Raspberry Pi 3 Model B with a locally built MSP430 GCC
4.7.0. Both use Contiki-NG commit
`713e72f545e15f0ea9eb00edd7e11dda855a7444` and Cooja submodule commit
`cc31dbd202f6ea9cf82f83aab240a4074eae8af9`.

The installers provide these two toolchain routes and select a private Temurin
JDK 21.0.8+9, matching the Java toolchain required by the pinned Cooja build.

### Obtain the project

Install Git in an existing Ubuntu/Debian system first:

```bash
sudo apt update
sudo apt install git
cd ~
git clone https://github.com/GeorgeKorillos/tsch-dbscan-fusion.git thesis
cd ~/thesis
```

The application repository and Contiki-NG are separate checkouts:

```text
~/thesis/       application, traces, tests, results and setup tools
~/contiki-ng/   pinned operating system and Cooja
```

For WSL, use Ubuntu's Linux filesystem, such as `~/thesis`, rather than a Windows
mounted directory. A graphical session is required to display Cooja: use WSLg or
an existing X server on WSL, or a desktop session on the Pi. Installing Windows,
WSL or a desktop operating system is outside the scripts' scope.

### Automatic setup on Ubuntu / WSL

Run as your normal user; the installer requests `sudo` for package installation:

```bash
cd ~/thesis
bash tools/setup/setup-ubuntu.sh
source ~/opt/thesis-tools/activate.sh
```

The script installs host build tools, Python analysis dependencies, graphical
runtime libraries, the legacy MSP430 compiler, Java, Contiki-NG and Cooja. The
MSP430 archive contains 32-bit Intel executables, so the script also installs
compatibility libraries on the 64-bit Ubuntu host. It does not install a separate
system Gradle or Ant. It builds Cooja with its own Gradle wrapper without opening
the GUI.

The default paths are:

| Location | Purpose |
| --- | --- |
| `~/opt/thesis-tools/msp430-4.7.2` | Ubuntu MSP430 toolchain |
| `~/opt/thesis-tools/jdk-21.0.8-x64` | Private Java installation |
| `~/.cache/thesis-setup` | Verified downloads and build workspace |
| `~/contiki-ng` | Contiki-NG, beside the project directory |
| `~/opt/thesis-tools/activate.sh` | PATH, JAVA_HOME and CONTIKI settings |

A single `source` line is added to `~/.bashrc` for new interactive terminals.
Source the activation file in the current terminal as shown above. This selects
the private Java installation without changing the system's Java alternatives.

### Automatic setup on Raspberry Pi

Use a 64-bit Debian 13 / Raspberry Pi OS 13 installation (`uname -m` must print
`aarch64`). The Pi environment uses Debian 13.1; the script does not support a
32-bit `armv7l` system.

```bash
cd ~/thesis
bash tools/setup/setup-raspberry-pi.sh
source ~/opt/thesis-tools/activate.sh
```

This route builds GMP 4.3.2, MPFR 2.4.2, MPC 0.8.1, binutils 2.22 and MSP430 GCC
4.7.0, then installs msp430mcu 20130321 and msp430-libc 20120716. The compiler
runs on AArch64 and generates MSP430 code. 

Compilation uses one job by default. On a Pi 3, keep that setting unless memory
availability has been checked. The compiler build requires at least 6 GiB free
in the build workspace; this is a conservative setup guard, not a measured peak
requirement. Allow additional room for installed tools, Cooja and its Gradle
cache. 

An external Linux filesystem can hold the build workspace:

```bash
bash tools/setup/setup-raspberry-pi.sh \
  --work-dir /media/george/build-disk/thesis-setup
```

Use your own mounted path and ensure it is writable. The installer never deletes
an existing toolchain or changes swap automatically. The original Pi work used
temporary swap. If memory is exhausted during a new build, stop other programs
and consider a temporary swap file on a filesystem that supports it. A dedicated
file avoids overwriting the system's existing swap:

```bash
# Create only a new dedicated swap file; stop if any step fails.
(
  set -e
  test ! -e /var/tmp/thesis-build.swap
  sudo dd if=/dev/zero of=/var/tmp/thesis-build.swap bs=1M count=2048
  sudo chmod 600 /var/tmp/thesis-build.swap
  sudo mkswap /var/tmp/thesis-build.swap
  sudo swapon /var/tmp/thesis-build.swap
)
```

After the build, close memory-heavy applications before releasing it:

```bash
sudo swapoff /var/tmp/thesis-build.swap && sudo rm /var/tmp/thesis-build.swap
```

No `/etc/fstab` entry is needed for temporary swap. Swap may make compilation
slow and does not replace free disk space for the build itself.

### What setup changes, and what it leaves to the experiment workflow

Setup installs development dependencies and builds Cooja. It does not generate
`include/sensor_traces.h`, compile any application profile, run tests or
simulations, collect version reports, or launch Cooja. Follow the build and
experiment instructions below after installation.

The scripts accept `--prefix`, `--work-dir`, `--contiki-dir`, `--jobs` and
`--stage`. Paths must not contain whitespace because the legacy makefiles do not
reliably handle it. The default Contiki directory is beside the cloned project;
`activate.sh` also exports its absolute path through `CONTIKI`.

```bash
bash tools/setup/setup-ubuntu.sh --help
```

Existing Contiki checkouts must already be at the pinned commit. If the revision
differs, setup stops rather than replacing local work. Use another
`--contiki-dir` or move the existing checkout aside. Download checksums are
verified before extraction. A failed build can be resumed with the same options;
completed compiler components are skipped. If the recipe or prefix changes,
use a new workspace and prefix instead of reusing incompatible objects.

Only Cooja and its recursive dependencies are initialized as submodules for this
Z1 workflow. The unrelated ARM SDKs are not needed. To develop for another
Contiki platform later, initialize its required submodules explicitly.

### Manual setup: host packages and downloads

The manual route runs the same operations separately, so a failure can be
examined before continuing. The stage interface is also useful when an
administrator installs packages but an ordinary user owns the compiler build.
Always use the same path options in all stages.

For Ubuntu:

```bash
bash tools/setup/setup-ubuntu.sh --stage packages
bash tools/setup/setup-ubuntu.sh --stage downloads
bash tools/setup/setup-ubuntu.sh --stage toolchain
bash tools/setup/setup-ubuntu.sh --stage java
bash tools/setup/setup-ubuntu.sh --stage contiki
bash tools/setup/setup-ubuntu.sh --stage cooja
bash tools/setup/setup-ubuntu.sh --stage environment
source ~/opt/thesis-tools/activate.sh
```

For Pi, replace `setup-ubuntu.sh` with `setup-raspberry-pi.sh` in those commands.
The following sections explain the underlying steps rather than treating the
stage interface as an opaque installer.

The packages stage installs the C/C++ host compiler, Make, Git, curl, archive
utilities, patch, flex, bison, ncurses development headers, zlib development
headers, Autotools, Python 3, pyserial, Matplotlib, srecord and Java GUI libraries.
The application's trace and metric tools use the Python standard library;
Matplotlib is used by the thesis plot scripts.

The file `tools/setup/versions/downloads.tsv` contains each filename, SHA-256
checksum and download URL. To download one dependency manually, copy its URL
and checksum from that file:

```bash
mkdir -p ~/.cache/thesis-setup/downloads
cd ~/.cache/thesis-setup/downloads
curl -fL '<URL_FROM_DOWNLOADS_TSV>' -o '<FILENAME>'
printf '%s  %s\n' '<SHA256_FROM_DOWNLOADS_TSV>' '<FILENAME>' | sha256sum -c -
```

System packages are installed from the host distribution repositories; their
individual patch versions are not frozen by this archive manifest.

These angle-bracket fields describe values to substitute from the manifest;
they are not extra repository metadata to invent. A checksum mismatch must be
resolved before extraction. GNU and MSP430 archive hashes were calculated from
the retrieved archives; JDK hashes come from the publisher's checksum files.
This pins the chosen bytes but does not constitute a separate publisher
signature verification.

### Manual Ubuntu toolchain and Java installation

After the downloads stage, the two verified archives can be extracted directly:

```bash
mkdir -p ~/opt/thesis-tools/msp430-4.7.2
mkdir -p ~/opt/thesis-tools/jdk-21.0.8-x64
tar --no-same-owner -xf ~/.cache/thesis-setup/downloads/mspgcc-4.7.2-compiled.tar.bz2 \
  --strip-components=1 -C ~/opt/thesis-tools/msp430-4.7.2
tar --no-same-owner -xf ~/.cache/thesis-setup/downloads/OpenJDK21U-jdk_x64_linux_hotspot_21.0.8_9.tar.gz \
  --strip-components=1 -C ~/opt/thesis-tools/jdk-21.0.8-x64
export JAVA_HOME="$HOME/opt/thesis-tools/jdk-21.0.8-x64"
export PATH="$HOME/opt/thesis-tools/msp430-4.7.2/bin:$JAVA_HOME/bin:$PATH"
```

Keep the compiler's directory tree intact. Its executables locate device
headers, libraries and linker scripts relative to their installation prefix.
If a compiler executable reports a missing shared library, use `ldd` on that
executable and install the corresponding compatibility package. Do not create
symlinks between unrelated ncurses library versions.

### Manual Pi toolchain construction

The following commands build the compiler manually after the package and
download stages. Run them in the same Bash terminal, using a fresh source and
build directory. Stop on any error before continuing to the next block.

#### Paths and extraction

```bash
cd ~/thesis
SETUP_DIR="$PWD/tools/setup"
CACHE="$HOME/.cache/thesis-setup/downloads"
SOURCES="$HOME/.cache/thesis-manual/sources"
BUILD="$HOME/.cache/thesis-manual/build"
PREREQS="$HOME/opt/thesis-tools/msp430-prerequisites"
TOOLCHAIN="$HOME/opt/thesis-tools/msp430-4.7.0"
mkdir -p "$SOURCES" "$BUILD" "$PREREQS" "$TOOLCHAIN"

for name in gmp-4.3.2 mpfr-2.4.2 binutils-2.22 gcc-4.7.0 \
  mspgcc-20120911 msp430mcu-20130321 msp430-libc-20120716; do
  tar --no-same-owner -xf "$CACHE/$name.tar.bz2" -C "$SOURCES"
done
tar --no-same-owner -xf "$CACHE/mpc-0.8.1.tar.gz" -C "$SOURCES"
tar --no-same-owner -xf "$CACHE/gcc-4.7.0-patches.tar.xz" -C "$SOURCES"
```

Stop on any error. Do not continue with partially extracted sources.

#### Host recognition and compiler patches

```bash
for name in gmp-4.3.2 mpfr-2.4.2 mpc-0.8.1 binutils-2.22 gcc-4.7.0; do
  find "$SOURCES/$name" -type f \
    \( -name config.guess -o -name configfsf.guess \) \
    -exec cp "$CACHE/config.guess" {} \;
  find "$SOURCES/$name" -type f -name config.sub \
    -exec cp "$CACHE/config.sub" {} \;
done
patch -d "$SOURCES/binutils-2.22" -p1 \
  < "$SOURCES/mspgcc-20120911/msp430-binutils-2.22-20120911.patch"
patch -d "$SOURCES/gcc-4.7.0" -p1 \
  < "$SOURCES/mspgcc-20120911/msp430-gcc-4.7.0-20120911.patch"
for name in PR-54638 gperf libiberty-multilib; do
  patch -d "$SOURCES/gcc-4.7.0" -p1 < "$SOURCES/gcc-4.7.0_$name.patch"
done
patch -d "$SOURCES/gcc-4.7.0" -p1 \
  < "$SETUP_DIR/patches/msp430-value-index.patch"
```

Apply each patch once. A rejected hunk means the source tree needs inspection;
forcing the patch is not a substitute for that inspection.

#### Build prerequisites, binutils and GCC

Run this block in a subshell so the compatibility flags end with the build. It
stops on the first failing command and uses one job to limit memory pressure.

```bash
(
  set -e
  export CC=gcc CXX=g++
  export CFLAGS='-O2 -std=gnu89 -Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=return-mismatch'
  export CXXFLAGS='-O2 -std=gnu++98'
  export CC_FOR_BUILD="gcc $CFLAGS"
  export MAKEINFO=true

  mkdir -p "$BUILD/gmp" "$BUILD/mpfr" "$BUILD/mpc"
  cd "$BUILD/gmp"
  "$SOURCES/gmp-4.3.2/configure" --build=aarch64-unknown-linux-gnu \
    --host=aarch64-unknown-linux-gnu --prefix="$PREREQS" --disable-shared
  make -j1
  make install

  cd "$BUILD/mpfr"
  "$SOURCES/mpfr-2.4.2/configure" --build=aarch64-unknown-linux-gnu \
    --host=aarch64-unknown-linux-gnu --prefix="$PREREQS" \
    --with-gmp="$PREREQS" --disable-shared
  make -j1
  make install

  cd "$BUILD/mpc"
  "$SOURCES/mpc-0.8.1/configure" --build=aarch64-unknown-linux-gnu \
    --host=aarch64-unknown-linux-gnu --prefix="$PREREQS" \
    --with-gmp="$PREREQS" --with-mpfr="$PREREQS" --disable-shared
  make -j1
  make install

  mkdir -p "$BUILD/binutils" "$BUILD/gcc"
  export PATH="$TOOLCHAIN/bin:$PATH"
  cd "$BUILD/binutils"
  "$SOURCES/binutils-2.22/configure" --target=msp430 \
    --program-prefix=msp430- --prefix="$TOOLCHAIN" \
    --build=aarch64-unknown-linux-gnu --host=aarch64-unknown-linux-gnu \
    --disable-nls --disable-werror
  make -j1 MAKEINFO=true
  make MAKEINFO=true install

  cd "$BUILD/gcc"
  "$SOURCES/gcc-4.7.0/configure" --target=msp430 --enable-languages=c \
    --program-prefix=msp430- --prefix="$TOOLCHAIN" \
    --build=aarch64-unknown-linux-gnu --host=aarch64-unknown-linux-gnu \
    --disable-nls --disable-werror --disable-shared \
    --with-gmp="$PREREQS" --with-mpfr="$PREREQS" --with-mpc="$PREREQS"
  make -j1 MAKEINFO=true
  make MAKEINFO=true install
)
```

#### Device package and target C library

After the previous block completes successfully:

```bash
(
  set -e
  export PATH="$TOOLCHAIN/bin:$PATH"
  cd "$SOURCES/msp430mcu-20130321"
  export MSP430MCU_ROOT="$PWD"
  bash scripts/install.sh "$TOOLCHAIN/"
  cd "$SOURCES/msp430-libc-20120716/src"
  unset CC CXX CFLAGS CXXFLAGS CC_FOR_BUILD
  make -j1
  make PREFIX="$TOOLCHAIN" install
)
```


For Pi's Java installation, extract the manifest's
`OpenJDK21U-jdk_aarch64_linux_hotspot_21.0.8_9.tar.gz` with one leading directory
removed, into `~/opt/thesis-tools/jdk-21.0.8-aarch64`, and export that directory
as `JAVA_HOME`.

### Manual Contiki-NG and Cooja setup

Starting with no `~/contiki-ng` checkout:

```bash
cd ~
git clone https://github.com/contiki-ng/contiki-ng.git
cd ~/contiki-ng
git checkout --detach 713e72f545e15f0ea9eb00edd7e11dda855a7444
git submodule update --init --recursive -- tools/cooja
patch -p1 < ~/thesis/tools/setup/patches/contiki-neighbor-initializer.patch
```

Apply the initializer patch only once. Its only change is
`linkaddr_t lladdr = {0};` to `linkaddr_t lladdr = {{0}};`, preserving zero
initialization. The automatic route detects an already-applied patch.

With Java 21 selected in `JAVA_HOME` and on PATH:

```bash
cd ~/contiki-ng/tools/cooja
bash ./gradlew --no-daemon --max-workers=1 \
  -Dorg.gradle.jvmargs=-Xmx512m assemble
```

This downloads Gradle and Java dependencies and builds Cooja without launching
it. Ant is not used. Do not use `sudo` for Gradle or compiler builds.

For a fully manual installation, save the PATH, JAVA_HOME and CONTIKI exports in
a shell file and source it from `~/.bashrc`. Alternatively, the `environment`
stage writes that file using the selected paths.

### Build the application and start an experiment

These commands are deliberately separate from environment setup:

```bash
source ~/opt/thesis-tools/activate.sh
cd ~/thesis
mkdir -p results
bash tools/build_profile.sh shared-adaptive 3 measure > results/build.txt 2>&1
# Continue only if the previous command succeeded.
cd ~/contiki-ng/tools/cooja
./gradlew run
```

Use the simulation and recording instructions below to select a configuration
and collect its outputs. The installer does not change seeds, profiles, radio
parameters or existing results.

Keep `include/sensor_traces.h` for the evaluated workload. The trace generator's
interface is `python3 generate_traces.py`, with an optional input path and
`--output` argument. However, its current nonnegative-int16 validation rejects
negative entries in unused groups of the original CSV. If the evaluated header
is missing, restore it from the repository; do not silently remove those checks
or overwrite the header with a different conversion. See the trace section and
`docs/dbscan.md` for the data representation.

### Setup files and troubleshooting

`common.sh` contains shared operations. `patches/` documents the small local
source corrections; larger upstream patches are downloaded with pinned hashes.
`versions/` identifies dependencies, not old application releases.

| Symptom | Action |
| --- | --- |
| Existing Contiki checkout has another revision | Use a new directory; preserve local edits before changing the old checkout. |
| Checksum mismatch | Remove only the named cached archive and download again. Do not bypass verification. |
| Compiler terminated during Pi build | Check memory and free space; keep one build job and consider temporary swap. |
| Missing i386 loader or shared library on Ubuntu | Install the compatibility package for the reported library. |
| Java toolchain not found | Source `activate.sh`; check JAVA_HOME points to the private JDK 21. |
| Cooja cannot open a display | Run within a graphical session or configure WSL's display support. |
| Firmware ROM overflow | Inspect section layout and build flags; do not enlarge `memory.x`. |
| Interrupted archive extraction | Move aside the named incomplete source directory and retry. |

The MSP430F2617 word at `0xFFBE` is reserved for the BSL security key. The
`0xcebe` ROM length is retained; it is not an off-by-two workaround target.
See TI application report [SLAA380B](https://www.ti.com/lit/pdf/slaa380).

The selected experimental results were produced with the application and
recorded environment documented here, before the installation workflow was
automated.


## Build a named profile

From the project root:

```bash
mkdir -p results
bash tools/build_profile.sh shared-adaptive 3 measure > results/build.txt 2>&1
```

A successful build creates:

- `src/master/build/z1/master_node.z1`
- `src/slave/build/z1/slave_node.z1`
- `src/sensor/build/z1/sensor_node.z1`

The script cleans and builds all three roles with explicit flags, stops on the
first failed command, prints their sizes, and prints the flags needed if Cooja
recompiles. Run one simulation before building the next profile: every profile
uses these same firmware paths. Do not mix firmware from different builds.

The script can also be launched from another working directory; it locates the
project through its own path. It does not modify source files or simulation
settings. `SIZE` can select another size utility if the host `size` cannot read
MSP430 binaries, for example `SIZE=msp430-size`.

### Main profiles

| Profile | Length | Schedule | EB policy | Sampling and reporting |
| --- | --- | --- | --- | --- |
| `shared-adaptive` | 3 or 5; default 3 | One shared TX/RX cell per frame | Infrastructure interval grows from 4 to 8 to 16 s; sensors do not advertise | Sample-delay guard; 4.75 s master deadline |
| `receiver-fixed` | 11, 17 or 31; default 17 | Common control cells and address-derived receiver cells | Infrastructure interval fixed at 4 s; sensors do not advertise | Same sample guard/deadline; uplink jitter in dynamic mode |

Both use compact application logging. The scheduler is selected at build time;
within a receiver-based build, TX cells are created as destinations are used.
It does not require a table of node roles or a known node count.

```bash
bash tools/build_profile.sh shared-adaptive 5 measure
bash tools/build_profile.sh receiver-fixed 11 measure
bash tools/build_profile.sh receiver-fixed 17 diagnose
bash tools/build_profile.sh --help
```

### Other comparison profiles

These profiles keep other behavioral switches available. They retain known
limitations and should not be selected just because they compile.

| Profile | Schedule / length | EBs | Extended timing / leaf EBs | Sample guard |
| --- | --- | --- | --- | --- |
| `shared-fixed-reference` | Shared / 3 | Fixed | Original timing; leaf EBs on | Off |
| `shared-adaptive-reference` | Shared / 3 | Adaptive | Original timing; leaf EBs on | Off |
| `receiver-fixed-reference` | Receiver / 11, 17, 31 | Fixed | Original timing; leaf EBs on | Off |
| `receiver-adaptive-reference` | Receiver / 11, 17, 31 | Adaptive | Original timing; leaf EBs on | Off |
| `shared-aligned` | Shared / 3 | Adaptive | Extended timing; leaf EBs off | Off |
| `receiver-fixed-aligned` | Receiver / 11, 17, 31 | Fixed | Extended timing; leaf EBs off | Off |
| `receiver-adaptive-aligned` | Receiver / 11, 17, 31 | Adaptive | Extended timing; leaf EBs off | Off |

Original timing uses a four-second master deadline. Extended timing uses 4.75 s
and spreads dynamic uplinks over up to one receiver-frame period. The five-second
cycle, 1.5-second collection phase and two-second peer phase stay unchanged.
Reference and aligned profiles keep verbose application logs in `measure` mode;
`diagnose` forces compact logging to leave space for diagnostics. For energy
comparisons, record the actual logging settings rather than assuming all profiles
have the same logging overhead.

The other shared profiles accept only length `3` (main shared profile accepts either `3` or `5`); omit the length or pass `3`.
Receiver profiles default to `17`.

The build script implements nine profiles: two main profiles and seven comparison profiles.

### Measure and diagnose modes

`measure` sets `PIPE_DIAGNOSTICS=0`. It retains the normal application report,
transmission and timing messages. It is not a silent build.

`diagnose` sets `PIPE_DIAGNOSTICS=1` and `PIPE_COMPACT_LOG=1`. It adds application
RX, coalescing and slave snapshot records. Use it to investigate delivery and
window boundaries, not as a substitute for the corresponding measurement build.
Use the third positional argument.

## Makefiles and individual builds

Each node Makefile names its main source and role-specific common modules.
`mk/node.mk` supplies common flags, the Contiki path and the TSCH/IPv6/nullrouting
selection. Sensors do not link the clustering or signal-transform modules.

Direct builds default to the current `shared-adaptive` configuration,
length 3, with CSV input and DBSCAN (MinPts=1). Named profiles still pass every flag explicitly.

| Root command | Action |
| --- | --- |
| `make` | Clean, build the default configuration and write `output_prof.txt` |
| `make all` | Build the three roles without cleaning first |
| `make clean` | Remove role build outputs and `output_prof.txt` |
| `make rebuild` | Finish cleaning before building all roles |
| `make profile` | Inspect existing binaries and write the memory report |
| `make ramprof` | Request per-role RAM symbol reports from Contiki |
| `make flashprof` | Request per-role flash symbol reports from Contiki |

Use a clean build after changing compile-time flags. GNU Make does not reliably
infer that an existing object was compiled with different command-line flags.
Running plain `make` after a named-profile build selects the direct-build defaults;
it does not remember the last profile.

An individual build, using the default profile settings:

```bash
make -C src/slave TARGET=z1 clean
make -C src/slave TARGET=z1 all
make -C src/slave TARGET=z1 viewconf
```

For another profile, reuse the full flag list printed by the profile script.
`viewconf` must also receive that list if its output is intended to describe the
nondefault build. It shows the current make invocation, not saved firmware metadata.

`USE_DBSCAN=1` selects local DBSCAN with MinPts=1 and weighted cluster
representatives. All named profiles enable it. `USE_DBSCAN=0` selects a single
weighted average over all inputs, without neighborhood-based grouping.
`USE_CSV_TRACES=1` uses the project's scalar traces; `0` uses synthetic signal
samples and eight-bin DFT magnitudes. `SENSOR_ID_OFFSET=5` maps Cooja sensor ID 5
to trace row zero. The profile script explicitly fixes these three options for
the existing comparisons; use a clean direct build for other combinations.

## Start the Cooja simulation

Build all roles first. With the sibling directory layout:

```bash
cd ~/contiki-ng/tools/cooja
./gradlew run
```

Open `~/thesis/logs/experiment_seed101.csc` through Cooja's File menu.
The file loads existing firmware, using paths relative to its own location.
The paths in this package point one directory up from `logs/` to `src/`.
If you move the `.csc` into a deeper directory, adjust those paths yourself.

The setup contains one master (ID 1), three slaves (IDs 2–4) and three sensors
(IDs 5–7), with seed 101 and up to one second of startup delay. The UDGM transmit
range is 40 and interference range 50; configured TX/RX success ratios are both
1. These settings do not eliminate interference or scheduling contention.
Record positions, mote IDs, seed and radio parameters with each experiment.

The application IDs printed at startup are hashes of MAC addresses. They are
not the Cooja `ID:` values shown at the start of log lines. Preserve startup
output when saving diagnostic logs so the analyzer can map between them.

Check startup output for the chosen schedule, length, EB policy, diagnostics,
sample guard and compact logging. The startup field `PIPE timing=` reports the extended-timing flag. The words `Generated ... Unique Event(s)` are
retained for log compatibility; they count output clusters, not globally unique
events across epochs.

Keep a complete log from boot. If the Mote output buffer truncates the beginning,
increase the `<logoutput>` limit before starting or export through a suitable
logging facility. The included setting is the original 40000-line limit (but for simulations over 1 hour, i would recomend to increase it over 200000+).
Do not reload a simulation between two PowerTracker snapshots from one run.
Changing `<randomseed>` changes the simulation seed; a new seed requires a fresh
run from the beginning.

### Record one complete run

1. In Cooja choose **File → Open simulation**, then select
   `logs/experiment_seed101.csc`. If a firmware path cannot be found, select the
   corresponding precompiled `.z1` file from the build directory listed above.
2. Keep the simulation paused until its mote types all use that build. Do not
   invoke a default Cooja compile command for a nondefault profile: it would omit
   the explicit flags. The project configuration loads firmware directly.
3. Open **Mote output** (should already be open, but if not: **Tools → Mote output...**) and **PowerTracker** (**Tools → Mote radio duty cycle...**) from the simulation plugins/tools
   menu if they are not already open. Start the simulation from time zero.
4. Save the complete Mote output(In the **Mote output** window: **File → Save to file**) as `results/run_log.txt`, preserving timestamps
   and `ID:` prefixes. Save PowerTracker text at simulation time 300 s by pausing at exactly 05:00.000. You can use the multipliers to manipulate the speed or the **Step** button to go 1ms each step. Then click the **Print to console/Copy to clipboard** (which copies it) and paste it to
   `results/power_start.txt` and at 3600 s by pausing at exactly 1:00:00.000. You can use the multipliers to manipulate the speed or the **Step** button to go 1ms each step. Then click the **Print to console/Copy to clipboard** (which copies it) and paste it to `results/power_end.txt`. Each file
   must contain exactly one snapshot, including every mote's `MONITORED` and
   `ON` counters. Pause at each boundary before copying the output.
5. The difference between those counters describes the interval actually
   captured. If the snapshots are a few seconds late, record those actual times;
   do not label them exact five-minute and one-hour boundaries.
6. Keep `results/build.txt`, the `.csc`, and the raw output together. The analysis
   commands below produce summaries; preserve the raw files for verification.

To select another seed, close the simulation and edit the `<randomseed>101</randomseed>`
element in a copy of the `.csc`, then reopen that copy and start from zero. A seed
controls simulator pseudorandom choices; it does not make changed firmware,
logging or toolchains equivalent. Never change source or rebuild one role midway
through a run. A longer simulation is not a replacement for independent seeds.

## Analyze saved output

The tools read text files and print JSON to standard output. They do not modify
firmware, simulations or the input logs.

```bash
python3 tools/analyze_delivery.py results/run_log.txt --start-seconds 300 --end-seconds 3600 --expected-weight 3 > results/run_analysis.json

python3 tools/powertracker_interval.py results/power_start.txt results/power_end.txt > results/run_radio.json
```

The delivery window includes its start and excludes its end. The parser accepts
Cooja log timestamps in `minutes:seconds.milliseconds` form, as used by the saved
runs. A report is excluded if its declared event count and collected event lines
disagree. `expected-weight` must match the experiment's intended contributions
per report; it is not automatically inferred from topology.

For each report with weight `w` and expected weight `E`, deficit is `max(0,E-w)`
and excess is `max(0,w-E)`. Excess is never subtracted from another report's
deficit. No sensor/epoch attribution is carried in the final aggregates.

Optional hop delays match a received source/sequence/cluster key with its latest
logged send within 30 seconds. This is a receive-based latency summary, not a
complete sent-packet loss estimator. `unmatched_rx_records=0` means every parsed
RX found a recent TX; it does not mean every TX found an RX. The detailed TX-cohort
checks in the journal were performed separately. Use these RX-based summaries together with the TX records when diagnosing delivery;
they do not provide a complete end-to-end packet-delivery ratio.

PowerTracker input files must each contain one snapshot, including per-node
`MONITORED` and `ON` microsecond counters. The script ignores `AVG` lines and
rounded percentages, requires identical node sets and rejects invalid differences.
It computes `100 * sum(delta_ON) / sum(delta_MONITORED)`. This measures radio duty
cycle, not energy in joules, and excludes startup only if the first snapshot does.
The tool cannot establish run identity for you: keep both snapshots from the same
uninterrupted run.

## Data and code layout

| Path | Responsibility |
| --- | --- |
| `include/network_messages.h` | Wire layouts, ports, timing and bounded capacities |
| `include/sensor_traces.h` | Existing 10 × 405 fixed-point trace table |
| `src/sensor/sensor_node.c` | Discovery, migration and periodic sensor input |
| `src/slave/slave_node.c` | Parent/child management, snapshotting and peer fusion |
| `src/master/master_node.c` | Collection, clustering and final reporting |
| `src/common/pipeline_schedule.c` | Shared/receiver scheduling and EB interval control |
| `src/common/pipeline_phase.c` | Beacon phase stamping and queue-age correction |
| `include/pipeline_sample_timing.h` | Prevent a small phase correction from sending twice |
| `src/common/clustering.c` | DBSCAN neighborhood expansion and weighted representatives |
| `src/common/fft_processing.c` | Eight-point direct DFT and integer square root |
| `src/common/mac_hash.c` | Application ID derived from the link-layer address |
| `generate_traces.py` | Convert the original CSV export to a sensor-major C array |
| `tests/` | Host-only schedule, sample timing and tool regression checks |
| `docs/dbscan.md` | DBSCAN model, implementation and bounded-memory behavior |

The existing generated header is sufficient to build and was preserved numerically. Do not regenerate
or shorten it simply to gain ROM: doing so changes the workload.

If the original export is available, regenerate explicitly:

```bash
python3 generate_traces.py /trace/sensor_stat_9c9e1035-650d-470e-ac5a-5eb52948562d.csv --output include/sensor_traces.h
```

The converter skips seven header rows, reads ten semicolon-separated sensor
columns after the timestamp, accepts decimal commas, multiplies by 100 and
truncates toward zero. It rejects empty/malformed datasets and
values outside the nonnegative signed 16-bit DATA range before writing. Rows
become sensors and columns become successive samples. Sensors wrap around the
trace at its end. A longer trace table can prevent the firmware from fitting.

The DBSCAN implementation and its interface are documented in `docs/dbscan.md`.

## DBSCAN on measurement data

The clustering stage operates in measurement space. It does not need node
coordinates, radio distances or RSSI. In CSV mode each point contains one scalar
measurement. In signal mode it contains eight DFT magnitudes, and similarity is
measured by squared Euclidean distance over those dimensions.

### Neighborhood and minimum density

`SPATIAL_DEDUP_THRESHOLD_SQ` defines epsilon squared. Its value is 250000; with
scalar measurements stored in hundredths this means epsilon = 5.00 measurement
units. The local neighborhood includes equality at the boundary. Feature scale
matters: a threshold meaningful for scalar traces must not be assumed to have
the same interpretation for signal magnitudes.

MinPts is fixed at 1, counting the point itself. Every input is therefore a core
point, and an isolated measurement forms a singleton cluster. The purpose is to
retain isolated observations rather than classify them as density noise. The
algorithm expands each connected epsilon-neighborhood until no undiscovered
neighbor remains. For example, 20, 24 and 28 form one cluster at epsilon 5:
24 connects the endpoints even though 20 and 28 are farther apart than epsilon.
There is no guarantee that a cluster's diameter is at most epsilon.

### Implementation and representative values

`src/common/clustering.c` expands neighborhoods using a bounded queue of input
indices. Inputs are marked when enqueued so each is processed once. Distances
are calculated from the input records, not from a moving cluster centroid.
Each component is represented by a weighted average and a represented-sensor
count. Weight affects the representative, not the MinPts test. The average uses
integer division during accumulation, so small truncation differences can depend
on traversal order even when component membership does not.

The input pool holds at most five records and the output array holds at most
three clusters. When the output capacity is exhausted, remaining components are
not emitted and a warning is logged. These are implementation limits, not DBSCAN
properties. Aggregate weights are stored in one byte; a contribution that would
exceed 255 is rejected by the averaging helper.

### Relationship to forwarding and P2P fusion

A local record may already represent measurements merged by a child relay.
DBSCAN runs on the records available in that local snapshot. P2P subsequently
compares cluster representatives with its strict distance predicate and applies
its contribution-transfer protocol. This hierarchical process is not equivalent
to collecting every original observation and running one centralized DBSCAN.

A complete report weight measures retained contributions. It does not by itself
measure cluster accuracy, reconstruction error or unique sensor/epoch delivery.
Algorithm checks and network delivery checks address different properties.

### Build selection and tests

All named profiles build the same DBSCAN implementation. The profile name selects
scheduling, beacon policy and timing; it does not select another clusterer.
An explicit direct build with `USE_DBSCAN=0` provides an all-input weighted-mean
mode for comparisons. It is not a second DBSCAN implementation.

```bash
bash tests/run_tests.sh dbscan
```

These host tests exercise chains, permutations, threshold equality, singleton and
empty inputs, capacity limits and weighted means in scalar and vector builds.
They do not simulate radio delivery or measure MSP430 stack consumption.

The algorithm follows the MinPts=1 specialization of Ester, Kriegel, Sander and
Xu, *A Density-Based Algorithm for Discovering Clusters in Large Spatial
Databases with Noise*, KDD 1996, pp. 226–231. The epsilon-neighborhood and
connectivity definitions determine membership; the weighted representative is
the application's additional fusion step.

## How the pipeline behaves

Collection snapshots are scheduled 1.5 seconds after each local five-second
cycle starts. Peer exchange occupies the next two seconds. Unsuppressed clusters
are then sent toward the current parent. A relay may receive its children's
aggregates after its own snapshot and process them in the following cycle.
The master reports at the selected four- or 4.75-second deadline.

UDP callbacks enqueue into a fixed pool. At the snapshot, records are moved to
a processing list without a yield. New arrivals stay in the active list. This
separates batches; it does not create a queue per sampling epoch. Within a
snapshot, records with the same source/cluster are coalesced to the newer sequence.
A delayed record may therefore be delivered successfully and still never appear
in a report. Clocks are softly aligned using application beacons stamped with
TSCH network uptime; no hard global sample epoch is transmitted.

Peer SNOOP is multicast; MERGE is a targeted unicast contribution. The receiver
checks its window token and records source/cluster pairs to avoid counting a
repeat twice. A sender freezes the contribution and may repeat it once on a
matching SNOOP. There is no application-level acceptance ACK. Once it suppresses
its own cluster, it cannot safely fall back to an upward copy without additional
provenance, because the peer may already have accepted the transfer.

The receiver scheduler hashes full MAC addresses into cells, reserving two
control offsets. Multiple senders to one receiver still share that receiver's
cell. Hash collisions are also possible. Packet metadata pins ordinary unicast
to its receiver cell; common-cell fallback is used when cell creation fails,
not as a universal second transmission opportunity. Unused TX cells expire
conservatively only while the global TSCH queue is empty.

Adaptive EB control changes the configured interval from 4 to 8 to 16 seconds,
with 60 seconds between increases and resets on relevant topology events. It is
an application-managed interval ramp, not a full Trickle implementation with
redundancy suppression. Disabling leaf EBs still permits reception, association
and synchronization; it assumes the advertising infrastructure is connected.

## Limits and troubleshooting

- Z1 ROM is tight. Only a successful target link confirms that firmware fits. `.data` occupies
  RAM and needs a ROM initializer; `size`'s `dec` column is not flash usage.
- `.data + .bss` does not include maximum stack usage. Fixed queues, neighbor
  tables and cluster caps limit scale even though the scheduler does not need a
  fixed node count.
- Keep `FFT_SAMPLES=8`: the lookup tables are specifically eight-point tables.
  Check intermediate arithmetic ranges when changing synthetic signal inputs.
  CSV mode is the measured workload.
- DBSCAN uses MinPts=1, so isolated inputs form singleton clusters. Buffer limits,
  output limits and integer truncation remain relevant; DBSCAN does not provide
  global duplicate detection or guarantee lossless delivery.
- A 16-bit application ID hash can collide. Sequence numbers are eight-bit;
  signed sequence comparisons rely on short gaps, not arbitrary long histories.
- `udp: no matching connection found` is expected when all-nodes multicast reaches
  a role without the destination UDP port. Slaves bind 8765/5678/5680, sensors
  8765 and the master 5678. Do not count these lines as lost sensor DATA. Unexpected
  unicast instances still deserve investigation.
- `PIPE COALESCE` reports replacement within a snapshot. If sequence numbers
  differ, this is not merely removal of an identical retransmission.
- `add packet failed` alone does not identify DATA loss: it may concern another
  queued frame. Match application TX/RX and inspect timing before assigning cause.
- Check effective route limits in `viewconf` and the linked symbols. The presence
  of `UIP_CONF_MAX_ROUTES=0` alone does not establish a RAM saving.


## Tests

The `tests/` directory contains host regression tests for the source.
Keep it with `src/`, `include/` and `tools/` so that implementation changes can
be checked without starting Cooja. It is not required to build or run firmware,
and no test source or stub is linked into the mote binaries.

### Requirements and one-command execution

The suite requires Linux or WSL, Bash, a host GCC compiler, Python 3 and standard
shell utilities. The Python tests use only the standard library. No Cooja,
Java, MSP430 compiler or downloaded Contiki checkout is needed for these checks.
The tests still require the project source and headers; the tests folder alone
is not a standalone distribution.

```bash
sudo apt install build-essential python3
cd ~/thesis
bash tests/run_tests.sh
```

The default runs every group and stops on the first compilation error or failed
assertion. Successful completion ends with `PASS: all test groups`. A nonzero
exit status means the suite did not pass. The runner resolves paths relative to
itself, so it can also be invoked by absolute path from another directory.

To save output, including compiler errors:

```bash
mkdir -p results
bash tests/run_tests.sh > results/host_tests.txt 2>&1
```

This is a test log, not a build profile or a simulation result. The tests do not
change `.z1` firmware, traces, profiles or simulation settings. Temporary host
executables are created in a separate test directory and removed on exit.

### Run an individual group

| Command | What it checks |
| --- | --- |
| `bash tests/run_tests.sh scheduler` | Scheduler packet metadata, receiver-cell creation/reuse, allocation fallback, cell cleanup and EB interval control |
| `bash tests/run_tests.sh timing` | Sample-delay guard with 16-bit and 32-bit clocks, including near-zero delays and timer wrap |
| `bash tests/run_tests.sh dbscan` | Local MinPts=1 neighborhood expansion and weighted representatives in scalar and vector builds |
| `bash tests/run_tests.sh tools` | Delivery-log and PowerTracker parser/analysis fixtures |
| `bash tests/run_tests.sh --help` | Command syntax and available groups |

`bash tests/run_dbscan_tests.sh` is a short wrapper for the DBSCAN group. It calls
the same runner and tests the same active source, rather than maintaining a
second implementation of the test procedure. `CC` can select another compatible
host C compiler; the default is `gcc`. Do not select `msp430-gcc`: these test
executables must run on the host computer.

### Coverage

**Scheduler and EB control.** The runner compiles the actual
`src/common/pipeline_schedule.c` with the master, slave and sensor configuration
headers. Its 144 configurations cover receiver lengths 11/17/31, dynamic
scheduling on/off, adaptive EBs on/off, extended timing on/off and leaf EBs
on/off. Tests check unicast versus common-cell selection, cell reuse, bounded
allocation fallback, safe cleanup while packets are queued, and the 4/8/16-second
EB ramp/reset behavior. This is a module configuration matrix, not 144 distinct
build profiles or radio simulations. In shared mode it checks the module's
shared-path behavior, not Contiki's internal minimal-schedule implementation.

**Sampling time.** `test_sample_timing.c` includes the actual
`pipeline_sample_timing.h` and verifies the guarded delay over a full five-second
period with `CLOCK_SECOND=128`. It checks preserved phase, minimum separation and
unsigned timer wrap for both clock widths. It does not run the sensor process or
measure end-to-end phase alignment over the radio.

**DBSCAN.** `test_dbscan.c` compiles the active `src/common/clustering.c` with
`USE_DBSCAN=1`. It checks a transitive chain, a reordered chain, equality at the
epsilon boundary, the strict P2P distance predicate, disconnected inputs, the
three-cluster output cap, empty/singleton input and weighted averaging. Both
scalar and eight-dimensional builds run; these fixtures vary the first
coordinate and do not exhaust arbitrary eight-dimensional vectors.

**Analysis tools.** `test_tools.py` imports the active tools under `tools/`.
Three fixtures check complete versus truncated reports, aggregate deficit,
matching an RX to a logged TX, counter differences, rejection of reversed
counter intervals and rejection of duplicate PowerTracker fields. Passing
fixtures do not establish that arbitrary input logs are complete or that
aggregate weight is an end-to-end packet-delivery ratio.

### Test layout and stubs

| Path | Purpose |
| --- | --- |
| `tests/run_tests.sh` | Main runner and group selection |
| `tests/run_dbscan_tests.sh` | Convenience wrapper for the DBSCAN group |
| `tests/test_schedule.c` | Scheduler/EB assertions and mock runtime |
| `tests/test_sample_timing.c` | Sample-delay arithmetic assertions |
| `tests/test_dbscan.c` | Clustering fixtures |
| `tests/test_tools.py` | Python analysis fixtures |
| `tests/scheduler_stubs/` | Minimal Contiki/TSCH interfaces for scheduler checks |
| `tests/timing_stubs/` | Clock types and timing constants for arithmetic checks |
| `tests/clustering_stubs/` | Byte-order and logging interfaces for clustering checks |

Stubs substitute only the APIs needed to run on the host. They are deliberately
small; they are not an emulation of Contiki-NG, the CC2420 or Z1. Production
modules are compiled directly from `src/` and `include/`, not copied into tests.

### When to run tests and what a pass means

Run the complete suite after changes to scheduling, timing, clustering, shared
headers or analysis tools, and before recording results for a new source
revision. Tests are useful project files; generated host executables are not.
The project uses `docs/dbscan.md` as its algorithm documentation.

A passing suite confirms only the assertions above. It does not compile the full
master/slave/sensor processes, exercise the P2P callback or application parent
migration, reproduce interference or packet loss, measure energy, establish
MSP430 ROM/stack headroom, or prove statistical reliability. A target build and
Cooja evaluation remain separate steps. Additional tests should exercise the
active implementation rather than a copied protocol function that can become
out of date.
