# TenebraOS

A custom **Devuan 5.0 (Daedalus)** live ISO with KDE Plasma desktop, built with `live-build`, and using **runit** as the init system.

- **Stable base like Debian** — Devuan Daedalus is binary-compatible with Debian 13 (Trixie), minus systemd.
- **runit as init** — `runit-init` is `/sbin/init`; services (dbus, NetworkManager, SDDM, …) are supervised by `runsvdir` from `/etc/sv/`.
- **Broad hardware/driver support** — full firmware set (free + non-free), AMD/Intel microcode, `dkms` + kernel headers, NVIDIA/AMD drivers installed automatically at setup.
- **Huge app support** — all ~59 000 Debian/Devuan packages, plus Flatpak and podman/distrobox (Arch containers).
- **T2 Mac support** — the installer detects Apple T2 hardware and installs the patched T2 kernel from the TenebraOS package repository, with the required kernel parameters — no special ISO needed.
- **amd64 everywhere** — the generic `linux-image-amd64` kernel boots on all 64-bit CPUs.

## Project structure

```
build.sh                  # Entry point: build ISO, test in QEMU, write to USB
build-packages.sh         # Builds the Tenebra custom .debs into repo/ + ISO includes
auto/config               # live-build `lb config` options (Devuan daedalus, runit/sysvinit)
packages/                 # Tenebra custom .deb packages (built by build-packages.sh)
│   ├── tenebra-wallpapers    # Plasma/SDDM wallpaper package
│   ├── tenebra-defaults      # live user, autostart, skel/plasma panel config
│   ├── tenebra-grub-theme    # GRUB boot menu theme
│   ├── tenebra-branding      # Calamares branding (logo, splash, slideshow)
│   └── tenebra-calamares     # Calamares settings + installer launcher
branding/                 # Calamares branding source (copied into the chroot)
calamares/                # Custom Calamares module source
│   └── modules/
│       ├── hardwaredetect   # GPU vendor + Apple T2 detection → globalStorage
│       ├── profileselect    # PyQt5 view: Gaming / Learning / Office picker
│       └── autoconfig       # Runs profile + hardware setup inside the installed system
repo/                     # TenebraOS apt repository (free GitHub hosting)
│   ├── publish-repo.sh   #   index generation + signing (apt-ftparchive or bundled python)
│   ├── upload-pool.sh    #   upload .debs as GitHub Release assets (needs gh CLI)
│   └── pkgs/             #   package build scripts (custom fastfetch, T2 kernel)
config/
├── package-lists/        # Packages in the live system
│   ├── desktop.list.chroot  # KDE Plasma, apps, runit/sysvinit, firmware, calamares
│   └── tenebra.list.chroot  # kernel, headers, dkms, tooling
├── hooks/normal/         # Scripts that run inside the chroot during build
│   ├── 0001-wallpaper.hook.chroot         # wallpaper file permissions
│   ├── 0002-calamares-autostart.hook.chroot  # autostart installer on first login
│   ├── 0003-sddm-theme.hook.chroot        # SDDM theme + wallpaper
│   ├── 0004-calamares-branding.hook.chroot   # remove Debian branding, install Tenebra
│   ├── 0005-calamares-modules.hook.chroot    # copy custom modules to Calamares
│   ├── 0006-plymouth-theme.hook.chroot    # Tenebra plymouth theme
│   ├── 0007-live-user.hook.chroot         # create/configure 'user'
│   ├── 0010-tenebra-packages.hook.chroot  # install custom .debs into the chroot
│   └── 0040-runit.hook.chroot             # runit as init + service wiring (see below)
├── bootloaders/          # Custom GRUB menu entry templates (grub-pc / grub-efi)
├── includes.binary/      # Files on the ISO (GRUB theme, hidden timeout)
└── includes.chroot/      # Files copied verbatim into the live system
    ├── etc/
    │   ├── sv/           # runit services: dbus, NetworkManager, sddm
    │   ├── calamares/settings.conf   # Calamares pipeline (see below)
    │   ├── os-release    # TenebraOS identity
    │   └── xdg/autostart/ + sudoers.d/ + skel/ …
    ├── branding/, calamares/modules/   # module/branding payloads for the 0004/0005 hooks
    ├── tenebra-src/profiles/           # post-install profile scripts (see below)
    ├── usr/share/keyrings/tenebraos-repo.gpg  # our apt repo key
    └── usr/share/sddm/themes/tenebra/  # SDDM theme
```

## Init system: runit

`runit-init` replaces `/sbin/init` (wired by `config/hooks/normal/0040-runit.hook.chroot`). Boot flow:

1. Kernel → `runit-init` → runit **stage 1** (`/etc/runit/1`): mounts filesystems, runs `/etc/rcS.d` scripts (live-config, keymaps, udev, …)
2. **stage 2** (`/etc/runit/2`): starts `runsvdir /etc/service`
3. Services in `/etc/sv/` (symlinked from `/etc/runit/runsvdir/default/`) are supervised and auto-restarted: `dbus`, `NetworkManager`, `sddm`

Add your own service with:

```sh
sudo mkdir -p /etc/sv/myservice
sudo tee /etc/sv/myservice/run <<'EOF'
#!/bin/sh
exec /usr/sbin/myservice --foreground
EOF
sudo chmod +x /etc/sv/myservice/run
sudo ln -s /etc/sv/myservice /etc/runit/runsvdir/default/
```

`sv status`, `sv up/down/restart`, `sv kill` manage services. Shutdown is `init 0` / `init 6`.

> `auto/config` sets `--initsystem sysvinit` because live-build itself only supports `sysvinit|systemd|none` (runit support was removed upstream). sysvinit provides the boot glue (stage-1 rcS scripts); runit takes over as PID 1 via `runit-init`. Adds `elogind` + `dbus` so Plasma works without systemd.

## TenebraOS package repository

Custom packages (custom fastfetch, T2 kernels, …) are served from GitHub Releases with a signed apt index in `repo/` — free hosting, no server needed:

```
deb [signed-by=/usr/share/keyrings/tenebraos-repo.gpg]
    https://github.com/iTzR1g/TenebraOS/releases/download/tenebraos-repo-pool/ ./
```

Publishing a package: `repo/pkgs/<pkg>/build.sh` → `repo/publish-repo.sh` → `repo/upload-pool.sh` → commit + push. See `repo/README.md`.

## T2 Mac (Apple T2 hardware)

`hardwaredetect` (Calamares) detects Apple Intel hardware; `autoconfig` then, inside the installed system:

1. installs the TenebraOS repository and `tenebraos-fastfetch`,
2. installs the T2-patched kernel package (`linux-image-*-t2-trixie`) from the repo,
3. adds `intel_iommu=on iommu=pt pm_async=off` to `GRUB_CMDLINE_LINUX` and runs `update-grub`.

The T2 kernel then boots by default (newest installed kernel). On any other machine the stock kernel is used and nothing extra is installed.

## How the Calamares installer pipeline works

```
[welcome] → [profileselect] → [locale] → [keyboard] → [partition] → [users] → [summary]

  ↓ (user clicks Install)

[partition] → [hardwaredetect] → [users] → [networkcfg] → [grubcfg] → [bootloader]
→ [hwclock] → [services] → [packages] → [autoconfig] → [umount] → [finished]
```

- `profileselect` asks Gaming / Learning & Development / Daily Use & Office → `globalStorage["usecase"]`.
- `hardwaredetect` silently detects GPU vendor (NVIDIA/AMD/Intel) and Apple T2 Macs → `globalStorage`.
- `autoconfig` copies `tenebra-src/profiles/*.sh` into the installed system, then chroots in to run the selected profile plus hardware setup (GPU drivers, TenebraOS repo, fastfetch, T2 kernel).

Each profile installs its app set: **gaming** (Steam, Lutris, Heroic, Wine, mangohud, Discord), **learning** (VS Code, Python, Node, Jupyter, VirtualBox, zram), **office** (LibreOffice, Thunderbird, GIMP, Inkscape, TLP).

## How to build

Prerequisites — a Debian or Devuan system with:

```sh
sudo apt install live-build debootstrap squashfs-tools xorriso qemu-system-x86 ovmf
```

Build:

```bash
sudo ./build.sh build      # Build the ISO (~20-40 min)
sudo ./build.sh test-qemu  # Boot in QEMU with UEFI
sudo ./build.sh flash      # Write ISO to USB
```

The ISO is `live-image-amd64.hybrid.iso`. `build.sh` calls `lb config` (options in `auto/config`), `build-packages.sh` (custom .debs), then `lb build`.

### Speed up builds

```sh
sudo apt install apt-cacher-ng
echo 'LB_APT_HTTP_PROXY="http://localhost:3142"' | sudo tee -a config/common
```

## Custom kernel

To build with a custom kernel:

1. Build kernel `.deb` packages on a Debian/Devuan system
2. Drop them into `config/packages.chroot/`
3. Rebuild — `lb build` will use your packages instead of the archive's