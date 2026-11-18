# MTplatitudes

A Linux distribution built entirely from source, not based on any other
distro. Terminal first: the only graphical program planned is the web browser.

> **Status:** milestone 1. It boots to a shell in an emulator. It does not
> install to a real disk yet. See [Roadmap](#roadmap).

## Design

- **Own layout.** The OS lives in `/core`, settings in `/settings`, people's
  files in `/users`. There is no `/bin`, `/usr`, `/lib` or `/home`, so software
  written for an ordinary Linux system does not find what it expects.
- **Own process 1.** `keeper` ([`src/keeper/keeper.c`](src/keeper/keeper.c),
  about 330 lines of C) is the first program the kernel runs. It starts the
  system, keeps the terminals running and shuts everything down. Boot policy
  is in a shell script it runs, so it can change without recompiling.
- **musl, statically linked.** No shared C library at runtime. Programs built
  for a glibc distro won't run here.
- **Minimal userland.** BusyBox provides the shell, core commands, `vi`, device
  handling and DHCP. No bash, Python or Perl.
- **Generic kernel.** Boot-critical drivers are built in. Hardware drivers
  (Wi-Fi, Intel graphics, sound, USB storage, touchpads) load at boot only if
  the hardware is present, and when a device is plugged in later.
- **Reproducible.** The whole OS is defined by this repository. Every push is
  built and boot-tested by GitHub Actions.

## Filesystem layout

| Path | What's there |
|---|---|
| `/core/cmd` | Every command |
| `/core/boot` | `startup`, `shutdown` and `checkup` scripts |
| `/core/net` | Networking scripts (`dhcp-lease`) |
| `/core/drivers` | Kernel drivers (modules) |
| `/core/firmware` | Device firmware (Wi-Fi, GPU) |
| `/core/share` | Shared data (the `welcome` message) |
| `/settings` | System settings: `hostname`, `terminals`, `about`, users, shell `profile` |
| `/users` | People's files (`/users/root` for root) |
| `/run`, `/tmp` | In-memory, wiped at every boot |
| `/dev`, `/proc`, `/sys` | Kernel interfaces (names fixed by the kernel) |
| `/etc` | Temporary compatibility link to `/settings`, because musl and some BusyBox commands look there. It will be removed. |

Inside `/settings`, the files `passwd`, `group`, `hosts`, `profile`,
`mdev.conf` and `resolv.conf` keep their standard names because the C library
and BusyBox look for them by name.

## How it boots

1. The kernel starts `/core/cmd/keeper`.
2. keeper mounts `/proc`, `/sys`, `/dev`, `/run` and `/tmp`.
3. keeper runs `/core/boot/startup`. That script sets the hostname, loads
   drivers for the hardware it finds and starts wired networking.
4. keeper starts a shell on each terminal listed in `/settings/terminals`, and
   restarts it if it exits.
5. `poweroff`, `reboot` and `halt` ask keeper to shut down. keeper runs
   `/core/boot/shutdown`, stops every process, syncs the disks and turns the
   machine off.

`/core/boot/checkup` tests that the system works. It runs automatically in CI
and you can run it by hand at any time.

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
./forge          # build everything (the kernel takes a while)
./forge test     # boot it in QEMU and run the checkup
./forge run      # boot it in QEMU and use it (Ctrl-A then X to quit)
```

Other commands: `./forge <part>` rebuilds one part, `./forge image`
reassembles the root filesystem only, and `./forge clean` deletes build output.

## Repository structure

| Path | What it is |
|---|---|
| `about.conf` | OS name, version, architecture, list of parts |
| `forge` | The build tool |
| `parts/` | One file per part of the OS: where to get it and how to build it ([details](parts/README.md)) |
| `config/linux/` | Kernel configuration on top of the upstream defaults |
| `config/busybox/` | BusyBox configuration changes |
| `src/keeper/` | keeper, our process 1 |
| `files/` | Files copied as-is into the OS (settings, scripts, welcome) |
| `.github/workflows/` | CI: build and boot test on every push |

## Roadmap

1. **Terminal.** Done in an emulator: kernel, keeper, shell, core tools, wired
   networking, driver autoloading and hotplug.
   Still to do: build our own cross-compiler instead of using the host's musl
   wrapper, and remove the `/etc` link.
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

Build scripts and our own code: to be decided. Each upstream part keeps its own
license (Linux and BusyBox are GPL-2.0).
