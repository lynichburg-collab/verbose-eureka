# Recipes

Every piece of the OS is described by one file here, `<name>.recipe`. It is a
small shell file that `build.sh` loads. Packages are listed, in build order, in
`PACKAGES` in `os.conf`.

## Fields

| Field | Meaning |
|---|---|
| `NAME` | Package name. Must match the file name. |
| `VERSION` | Version, used in the source directory name. |
| `SOURCE_GIT` | Git URL to fetch the source from. |
| `SOURCE_REF` | Tag or branch to check out. |
| `SOURCE_LOCAL` | Instead of fetching: a directory in this repo (used for code we write ourselves, like `src/init`). |
| `INPUTS` | Extra files or directories in this repo. If any of them change, the package is rebuilt. |

## Functions

`build()` compiles the package into `$BUILD`. It only runs when the recipe or
its `INPUTS` changed since the last successful build.

`install()` copies the result from `$BUILD` into the root filesystem at
`$ROOTFS`. It runs every time the image is assembled.

## Variables available to recipes

| Variable | Meaning |
|---|---|
| `$SRC` | Source directory |
| `$BUILD` | This package's build directory |
| `$ROOTFS` | The root filesystem being assembled |
| `$SYSROOT` | Kernel headers (`$SYSROOT/include`), installed by the linux recipe |
| `$OUT` | Final outputs (kernel image, initramfs) |
| `$ROOT` | The repository root |
| `$JOBS` | Parallel build jobs |
| `$TARGET_CC`, `$TARGET_CFLAGS`, `$TARGET_LDFLAGS` | Compiler and flags for programs that run on the OS |

Note: a recipe's `install()` function shadows the `install` command. Use
`command install ...` inside it.
