# TenebraOS — Bare-Source Build System

A complete LFS/Devuan-style build system for creating TenebraOS from bare source tarballs. No pre-assembled ISOs or live-build scripts — everything is compiled from upstream source code.

## Architecture

```
bare/
├── 01-bootstrap.sh          Phase 1: Btrfs partitioning + cross-toolchain + core userland
├── 02-kernel-initramfs.sh   Phase 2: Kernel compilation + Btrfs-aware initramfs + GRUB
├── 03-init-setup.sh         Phase 3: Init freedom (OpenRC/runit/s6) with service templates
├── 04-hybrid-pkgmanager.sh  Phase 4: Install Zebra (zbra) package manager
├── 05-snapshot-setup.sh     Phase 5: Snapper + grub-btrfs + dpkg transaction hooks
├── 06-master-build.sh       Phase 6: Final rootfs assembly + deployment tarball
├── 07-build-iso.sh          Phase 7: Bootable ISO builder (BIOS + UEFI hybrid)
├── usr-local-bin/
│   ├── tenebra-pkg          Legacy dual-engine package manager
│   └── zbra                 Zebra: multi-backend package manager (default)
└── README.md                This file
```

## Quick Start

### Build from scratch (on a Debian/Devuan host):

```bash
# Phase 1: Partition disk + build toolchain + core packages
sudo ./01-bootstrap.sh /dev/sdX

# Phase 2: Compile kernel + initramfs + GRUB
sudo ./02-kernel-initramfs.sh

# Phase 3: Set up init system (choose one)
sudo ./03-init-setup.sh runit    # or openrc, or s6

# Phase 4: Install Zebra package manager
sudo ./04-hybrid-pkgmanager.sh

# Phase 5: Snapshot management
sudo ./05-snapshot-setup.sh

# Phase 6: Finalize rootfs
sudo ./06-master-build.sh --target /dev/sdX

# Phase 7: Build bootable ISO
sudo ./07-build-iso.sh
```

### Build rootfs only (no target disk):

```bash
# Skip partitioning — builds into /tmp/tenebra-build/rootfs
sudo ./01-bootstrap.sh /dev/sdX --skip-toolchain
sudo ./02-kernel-initramfs.sh
sudo ./03-init-setup.sh runit
sudo ./05-snapshot-setup.sh
sudo ./06-master-build.sh
sudo ./07-build-iso.sh
```

### Build ISO options:

```bash
# Default (xz compression, output to /tmp/tenebra-build/)
sudo ./07-build-iso.sh

# Custom output path
sudo ./07-build-iso.sh --output /home/user/TenebraOS.iso

# Use gzip compression (faster build, larger ISO)
sudo ./07-build-iso.sh --compress gzip

# Use zstd compression (good balance)
sudo ./07-build-iso.sh --compress zstd

# Use custom rootfs
sudo ./07-build-iso.sh --rootfs /mnt/custom-rootfs
```

## Prerequisites

On a Debian/Devuan build host:

```bash
sudo apt install build-essential wget curl git \
    btrfs-progs parted dosfstools e2fsprogs \
    gawk bison flex texinfo gettext \
    libncurses-dev libssl-dev libelf-dev \
    libgmp-dev libmpfr-dev libmpc-dev \
    debootstrap grub-pc-bin grub-efi-amd64-bin \
    xorriso squashfs-tools cpio
```

## What Each Phase Does

### Phase 1: Bootstrap (`01-bootstrap.sh`)

- Partitions target disk with GPT (EFI + Btrfs root)
- Creates Btrfs subvolumes: `@` (root), `@home`, `@snapshots`, `@var_log`
- Downloads and compiles a cross-toolchain:
  - `binutils` (cross-assembler/linker)
  - `gcc` (C/C++ compiler, stage 1 + full)
  - `glibc` (C library)
- Builds core userland from source:
  - `make`, `bash`, `coreutils`, `util-linux`, `btrfs-progs`, `dbus`, `elogind`

### Phase 2: Kernel & Initramfs (`02-kernel-initramfs.sh`)

- Downloads and compiles the Linux kernel with TenebraOS config
- Generates a custom Btrfs-aware initramfs with `switch_root`
- Installs GRUB (EFI + BIOS) with boot configuration

### Phase 3: Init Freedom (`03-init-setup.sh`)

Three mutually exclusive init systems:

| Init    | PID 1              | Service Manager    | Best For                    |
|---------|--------------------|--------------------|-----------------------------|
| `runit` | `runit-init`       | `runsvdir`         | Lightweight, reliable        |
| `openrc`| sysvinit + OpenRC  | OpenRC             | Feature-rich, Gentoo-like    |
| `s6`    | `s6-linux-init`    | `s6-rc`            | Maximum control, containers  |

All systems include service templates for:
- `udevd` — device manager (eudev)
- `dbus` — system message bus
- `elogind` — session manager (login without systemd)
- `NetworkManager` — network management
- `sddm` — display manager
- `pipewire` — audio server

### Phase 4: Zebra Package Manager (`zbra`)

Zebra is TenebraOS's default multi-backend package manager that handles native `.deb` packages from GitHub while supporting passthrough to foreign package managers.

**Native mode** (GitHub packages):
```bash
zbra -i vim                          # Install from TenebraOS-packages
zbra -s firefox                      # Search native packages
zbra -b /path/to/source.tar.gz       # Build from source
zbra --snapshots                     # List Btrfs snapshots
zbra --rollback zbra_pre-install_*   # Rollback to snapshot
```

**Foreign backend passthrough**:
```bash
zbra -pm apt -i vim                  # Install via apt
zbra -pm pacman -i neovim            # Install via pacman (distrobox)
zbra -pm yay -i hyprland             # Install from AUR
zbra -pm flatpak -i firefox          # Install via Flatpak
zbra -pm snap -i spotify             # Install via Snap
```

**Source builds**:
```bash
zbra -b https://ftp.gnu.org/gnu/wget/wget-1.24.tar.xz
zbra -b git@github.com:user/repo.git
```

### Phase 5: Snapshots (`05-snapshot-setup.sh`)

- **Snapper**: Timeline snapshots every 30 minutes
- **Dpkg hooks**: Auto-snapshot before/after package operations
- **grub-btrfs**: Bootable snapshots in GRUB menu
- **Rollback**: Restore from GRUB boot menu or CLI

```bash
tenebra-pkg snapshot create "Before kernel update"
tenebra-pkg snapshot list
tenebra-pkg snapshot rollback 3
```

### Phase 6: Master Build (`06-master-build.sh`)

- Installs `zbra` (and legacy `tenebra-pkg`) into rootfs
- Generates `/etc/fstab` with partition UUIDs
- Creates deployment tarball: `tenebraos-rootfs.tar.xz`

### Phase 7: ISO Builder (`07-build-iso.sh`)

Creates a hybrid ISO (BIOS + UEFI) from the bare-source rootfs:

1. Copies kernel + initramfs to staging
2. Compresses rootfs into SquashFS (xz/gzip/zstd)
3. Sets up ISOLINUX for BIOS boot
4. Builds GRUB EFI image for UEFI boot
5. Assembles hybrid ISO with xorriso (BIOS + UEFI bootable)
6. Verifies ISO integrity

```bash
# Default build
sudo ./07-build-iso.sh

# Custom output + compression
sudo ./07-build-iso.sh --output /path/to/output.iso --compress zstd
```

## Configuration

### `/etc/tenebra/make.conf`

Global build configuration for source compilation:

```bash
CFLAGS="-O2 -pipe -march=x86-64 -mtune=generic"
CXXFLAGS="-O2 -pipe -march=x86-64 -mtune=generic"
MAKEOPTS="-j$(nproc)"
USE="X kde wayland alsa pulseaudio dbus elogind btrfs"
USE_DISABLE="systemd systemd-journal"
SRC_MIRROR="https://ftp.gnu.org/gnu"
FEATURES="ccache distcc"
```

### `/etc/tenebra/profiles/`

Build profiles that set USE flags for different use cases:

- `desktop` — X11, KDE, Wayland, audio, full desktop
- `server` — Minimal, headless, no GUI
- `gaming` — Steam, Wine, Vulkan, performance tuning
- `develop` — Compilers, debug tools, development headers
- `hardened` — Stack protector, FORTIFY, RELRO

## Snapshot Rollback

### From GRUB:
1. Boot → select "TenebraOS (snapshots)"
2. Choose a snapshot to boot
3. System boots into the read-only snapshot

### From CLI:
```bash
tenebra-pkg snapshot list
tenebra-pkg snapshot rollback 3    # restore snapshot #3
reboot
```

## Directory Structure

```
/tmp/tenebra-build/
├── sources/          Downloaded source tarballs
├── tools/            Cross-compilation toolchain
├── rootfs/           The target rootfs being built
├── logs/             Build logs for each package
├── builds/           Per-package build directories
├── initramfs-tmp/    Initramfs build staging
├── squashfs-root/    Temporary copy for squashfs creation
├── iso-staging/      ISO directory structure (kernel, squashfs, GRUB)
├── iso-build.log     xorriso build log
├── .kernel-release   Stored kernel version string
└── TenebraOS-*.iso   Final bootable ISO image

/etc/tenebra/
├── build.conf        Global build configuration
├── profiles/         Build profiles (*.conf)
└── init-*.available  Installed init system markers

/etc/sv/              runit service definitions
/etc/snapper/configs/ Snapper configuration
/boot/grub/           GRUB configuration + snapshots
/snapshots/           Btrfs snapshot storage
/var/cache/tenebra/   Package cache + source cache
/var/lib/tenebra/world  Source-built package registry
```

## Extending

### Adding a new source package:

1. Add the URL to `tenebra-pkg` or create a build script
2. Use `tenebra-pkg build <url>` with appropriate USE flags
3. The package is automatically recorded in the world file

### Adding a new init service:

1. Create a service directory in `/etc/sv/<name>/`
2. Add a `run` script (runit) or init script (OpenRC)
3. Symlink into `/etc/runit/runsvdir/default/` (runit) or use `rc-update add` (OpenRC)

### Custom kernel:

```bash
sudo ./02-kernel-initramfs.sh
# Edit kernel config before compilation:
cd /tmp/tenebra-build/linux-*
make menuconfig
# Then re-run the script
```

## Troubleshooting

### Build fails on package X:
- Check logs: `/tmp/tenebra-build/logs/<package>-*.log`
- Ensure all dependencies are installed on the build host
- Try re-running with `TENEBRA_JOBS=1` for more verbose output

### Rootfs is too small:
- Ensure the target disk has at least 20GB free
- Btrfs subvolumes share space dynamically

### GRUB doesn't appear:
- Ensure EFI partition is mounted at `/boot`
- Run: `grub-install --target=x86_64-efi --efi-directory=/boot`
- Run: `grub-mkconfig -o /boot/grub/grub.cfg`
