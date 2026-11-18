# Parts

Every piece of the OS is described by one file here, `<name>.part`. It is a
small shell file that `forge` loads. Parts are listed, in build order, in
`PARTS` in `about.conf`.

## Fields

| Field | Meaning |
|---|---|
| `NAME` | Part name. Must match the file name. |
| `VERSION` | Version, used in the source directory name. |
| `SOURCE_GIT` | Git URL to fetch the source from. |
| `SOURCE_REF` | Tag or branch to check out. |
| `SOURCE_LOCAL` | Instead of fetching: a directory in this repo (used for code we write ourselves, like `src/keeper`). |
| `INPUTS` | Extra files or directories in this repo. If any of them change, the part is rebuilt. |

## Patches

If `patches/<name>/` exists, every `*.patch` in it is applied to the
downloaded source (in name order) before building. Changing a patch resets
the source tree and triggers a rebuild. Example:
`patches/busybox/0001-mdev-run-commands-without-bin-sh.patch`.

## Functions

`build()` compiles the part into `$BUILD`. It only runs when the `.part` file
or its `INPUTS` changed since the last successful build.

`install()` copies the result from `$BUILD` into the root filesystem at
`$ROOTFS`. It runs every time the image is assembled.

## Variables available to parts

| Variable | Meaning |
|---|---|
| `$SRC` | Source directory |
| `$BUILD` | This part's build directory |
| `$ROOTFS` | The root filesystem being assembled |
| `$SYSROOT` | Kernel headers (`$SYSROOT/include`), installed by the linux part |
| `$OUT` | Final outputs (kernel image, initramfs) |
| `$ROOT` | The repository root |
| `$JOBS` | Parallel build jobs |
| `$TARGET_CC`, `$TARGET_CFLAGS`, `$TARGET_LDFLAGS` | Compiler and flags for programs that run on the OS |

Note: a part's `install()` function shadows the `install` command. Use
`command install ...` inside it.
