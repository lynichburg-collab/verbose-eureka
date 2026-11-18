# MTplatitudes

A Linux distribution built entirely from source, not based on any other
distro. Terminal first: the only graphical program planned is the web browser.

The name is "empty platitudes": like any good platitude, parts of this OS
sound impressive and do nothing. Some of that is a joke and some of it is a
trap (see [Platitudes](#platitudes) and [Tripwires](#tripwires)).

> **Status:** milestone 1. It boots to a shell in an emulator. It does not
> install to a real disk yet. See [Roadmap](#roadmap).

## Design

- **Own layout.** The OS lives in `/core`, settings in `/settings`, people's
  files in `/users`. There is no `/lib`, `/home` or `/bin/sh`, so software
  written for an ordinary Linux system does not find what it expects. `/bin`
  and `/usr` exist only to hold decoys.
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
| `/core/tripwire` | The decoy program, the bait list, the bait-file handler |
| `/settings` | System settings: `hostname`, `terminals`, `about`, users, shell `profile` |
| `/users` | People's files (`/users/root` for root) |
| `/bin`, `/usr/bin`, `/root` | Decoys and bait only. Never used by the OS. |
| `/var/log` | `activity` (everything that happened) and `tripwire` (decoy hits) |
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

`/core/boot/checkup` tests that the system works (27 checks). It runs
automatically in CI and you can run it by hand at any time.

## Activity log

`recorder` (started at boot) asks the kernel's audit system to report events
and writes one line per event to `/var/log/activity`:

| Kind | What's recorded |
|---|---|
| `run` | Every program started, with its arguments, user and parent. Failed and **blocked** attempts too. |
| `net` | Every outgoing connection (program and destination), and any program claiming a port or waiting for incoming connections. |
| `device` | USB devices and disks being attached or removed. |
| `session` | Shells starting and ending on each terminal. |
| `boot` | Startup, the recorder starting, shutdown. |

```
2026-11-18 23:09:12  net     uid=0 pid=798 (wget)  out -> 1.1.1.1:80
2026-11-18 23:09:16  run     uid=0 pid=800 parent=786  /tmp/sneaky  BLOCKED (not allowed to run here)
```

`activity` shows the last 40 entries; `activity all`, `activity follow`, or a
kind (`activity net`) for more. The log rotates at 4 MB.

Not recorded: what was *inside* connections (too large to keep), DNS lookups
on their own (the connection that follows them is), and shell built-ins like
`cd` or `echo`, which don't start a program.

## Lockdown

- `/core` and the decoy folders (`/bin`, `/usr`) are **read-only** while the
  system runs.
- **Nothing can run from places that hold data**: `/tmp`, `/run`, `/var`,
  `/settings` and `/root`. Downloading a program and running it fails, and the
  attempt is logged as `BLOCKED`. `/users` is left alone so your own scripts
  keep working.

Limits, honestly: someone with full root access can undo mounts, kill the
recorder or edit logs. The protections make that noisy and slower, not
impossible. While the OS runs from memory (milestone 1) the logs reset at
every boot. Planned, but not built because they add prompts to daily use: an
outbound network allowlist, and approval of sensitive actions only at the
physical keyboard.

## Platitudes

Commands that sound important, show a progress bar, report success and do
nothing at all: `optimize`, `boost`, `align`, `harmonize`, `defragment`,
`calibrate`, `reticulate`, `enlighten`. `wisdom` prints an empty platitude.
They are one script, `/core/cmd/platitude`, under several names.

## Tripwires

Attack scripts written for normal Linux reach for `bash`, `curl`, `python3`,
`sudo`, a stolen SSH key and so on. MTplatitudes keeps none of these, so it puts
decoys where they would be. Nothing legitimate ever touches them, so any use
at all is worth knowing about.

- **Decoy programs** at `/bin/bash`, `/usr/bin/{env,python3,perl,curl,wget,sudo}`.
  One small static program ([`src/decoy/decoy.c`](src/decoy/decoy.c)) acts
  like each of them: answers `--version`, accepts scripts and downloads, offers
  a fake shell, and has a `sudo` that rejects every password. It does nothing
  and logs what ran, its arguments and what started it. Passwords typed into
  the fake `sudo` are never logged.
- **Bait files**: `/root/.ssh/id_rsa`, `/settings/master.key` and
  `/settings/recovery-codes` look real but are random bytes generated at build
  time. `inotifyd` watches them and logs any open, change, move or delete.
- `/bin` and `/usr/bin` come *last* on the command path, after `/core/cmd`, so
  real commands always win and only names the OS doesn't have reach a decoy.
- The log is `/var/log/tripwire`. Log in after a decoy was touched and you get a
  warning; `tripwires` shows the log and `tripwires --clear` empties it.

Tripwires notice things; they don't stop a targeted attacker. Keeping the
kernel and browser up to date matters more. Expect a harmless hit if you type
`sudo` out of habit or search the whole disk with `grep -r`.

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
| `patches/<part>/` | Changes we make to upstream source code, applied after download |
| `parts/` | One file per part of the OS: where to get it and how to build it ([details](parts/README.md)) |
| `config/linux/` | Kernel configuration on top of the upstream defaults |
| `config/busybox/` | BusyBox configuration changes |
| `src/keeper/` | keeper, our process 1 |
| `src/decoy/` | The tripwire decoy program |
| `src/recorder/` | recorder, which writes the activity log |
| `files/` | Files copied as-is into the OS (settings, scripts, welcome) |
| `.github/workflows/` | CI: build and boot test on every push |

## Roadmap

1. **Terminal.** Done in an emulator: kernel, keeper, shell, core tools, wired
   networking, driver autoloading and hotplug, platitudes, tripwires,
   activity log, read-only system and no-run data folders.
   Still to do: build our own cross-compiler and C library instead of using
   the host's musl, patching its hardcoded `/etc` and `/bin/sh` paths, then
   remove the `/etc` link.
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
