# Verbose Eureka

A Linux distribution built entirely from source, not based on any other
distro. Terminal first: the only graphical program planned is the web browser.

> **Status:** milestone 1. It boots to a shell in an emulator. It does not
> install to a real disk yet. See [Roadmap](#roadmap).

## Design

- **Own layout.** Everything that ships with the OS is in `/system`. There is
  no `/bin`, `/usr`, `/sbin` or `/lib`, so software written for an ordinary
  Linux system does not find what it expects.
- **Own init.** Process 1 is [`src/init/init.c`](src/init/init.c), about 300
  lines of C. Boot policy is in a shell script it runs, so it can change without
  recompiling.
- **musl, statically linked.** No shared C library at runtime. Programs built
  for a glibc distro won't run here.
- **Minimal userland.** BusyBox provides the shell, core commands, `vi`, device
  handling and DHCP. No bash, Python or Perl.
- **Generic kernel.** Boot-critical drivers are built in. Hardware drivers
  (Wi-Fi, Intel graphics, sound, USB storage, touchpads) are modules that load
  at boot only if the hardware is present.
- **Reproducible.** The whole OS is defined by this repository. Every push is
  built and boot-tested by GitHub Actions.

## Filesystem layout

| Path | What's there |
|---|---|
| `/system/bin` | Every command |
| `/system/lib/boot` | Boot and shutdown scripts, self-test |
| `/system/lib/net` | Networking scripts |
| `/system/modules` | Kernel modules |
| `/system/firmware` | Device firmware (Wi-Fi, GPU) |
| `/system/share` | Shared data (motd) |
| `/config` | System configuration (hostname, consoles, users, profile) |
| `/home` | User files (`/home/root` for root) |
| `/run`, `/tmp` | In-memory, wiped at every boot |
| `/dev`, `/proc`, `/sys` | Kernel interfaces |
| `/etc` | Temporary compatibility link to `/config`, because musl and some BusyBox commands look there. It will be removed. |

## How it boots

1. The kernel starts `/system/bin/init`.
2. init mounts `/proc`, `/sys`, `/dev`, `/run` and `/tmp`.
3. init runs `/system/lib/boot/rc.boot`. That script sets the hostname, loads
   drivers for the hardware it finds and starts wired networking.
4. init starts a shell on each terminal listed in `/config/consoles`, and
   restarts it if it exits.
5. `poweroff`, `reboot` and `halt` ask init to shut down. init runs
   `rc.shutdown`, stops every process, syncs the disks and turns the machine
   off.

## Building

### On GitHub (no setup)

Every push runs the build and a boot test in the **Actions** tab. The kernel and
root filesystem are attached to each run as a downloadable artifact.

### On your own machine

You need a Linux machine (or WSL2 on Windows) with these packages. On
Debian/Ubuntu:

```sh
sudo apt-get install git build-essential musl-tools qemu-system-x86 bc flex \
    bison libelf-dev libssl-dev cpio rsync kmod
```

Then:

```sh
./build.sh          # build everything (the kernel takes a while)
./build.sh test     # boot it in QEMU and run the self-test
./build.sh run      # boot it in QEMU and use it (Ctrl-A then X to quit)
```

Other commands: `./build.sh <package>` rebuilds one package,
`./build.sh image` reassembles the root filesystem only, and `./build.sh clean`
deletes build output.

## Repository structure

| Path | What it is |
|---|---|
| `os.conf` | OS name, version, architecture, package list |
| `build.sh` | The build system |
| `recipes/` | One file per package: where to get it and how to build it ([details](recipes/README.md)) |
| `config/linux/` | Kernel configuration on top of the upstream defaults |
| `config/busybox/` | BusyBox configuration changes |
| `src/init/` | Our init |
| `rootfs/` | Files copied as-is into the OS (config, scripts) |
| `.github/workflows/` | CI: build and boot test on every push |

## Roadmap

1. **Terminal.** Done in an emulator: kernel, init, shell, core tools, wired
   networking, driver autoloading.
   Still to do: build our own cross-compiler instead of using the host's musl
   wrapper, and replace the `/etc` link.
2. **Installs on real hardware.** A bootable USB installer that installs
   alongside an existing OS, only using free space. Boots via UEFI. Includes
   firmware for common Wi-Fi cards and GPUs, Wi-Fi setup (iwd), and user
   accounts with a login prompt.
3. **Files.** A terminal file manager, automatic USB-drive mounting, and a
   filesystem choice for the install (ext4 or btrfs).
4. **Browser.** No desktop. Typing `web` opens a full-screen browser through a
   minimal Wayland kiosk compositor, and closing it returns to the terminal.
   Needs Mesa, input handling, fonts and audio.

## Licensing

Build scripts and our own code: to be decided. Each upstream package keeps its
own license (Linux and BusyBox are GPL-2.0).
