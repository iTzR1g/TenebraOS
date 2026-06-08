# TenebraOS

A custom Debian 13 (Trixie) live ISO with KDE Plasma desktop, built with `live-build`.

## How live-build works

`live-build` is a set of scripts that automates building Debian live system ISOs. It works in stages:

```
lb bootstrap  →  lb chroot  →  lb binary  →  lb source
    ↓               ↓             ↓
  debootstrap    install       package into
  base system    packages,     ISO image
                 config,       (squashfs +
                 hooks         isolinux/grub)
```

1. **bootstrap** — Creates a minimal Debian rootfs via `debootstrap`
2. **chroot** — Installs your selected packages, runs hooks, copies config files
3. **binary** — Squashes the chroot into a read-only filesystem, creates ISO with GRUB/isolinux

## Project structure

```
build.sh                     # Entry point: builds ISO, tests in QEMU, writes to USB

config/
├── package-lists/           # Packages to install in the live system
│   ├── base.list.chroot     #   Base packages (kernel, firmware, calamares, drivers)
│   └── live.list.chroot     #   Live desktop apps (KDE Plasma, media, office, calamares)
│
├── hooks/                   # Scripts that run inside the chroot during build
│   ├── normal/
│   │   ├── 0005-live-user.hook.chroot     # Renames the live user to 'user'
│   │   ├── 0030-calamares.hook.chroot     # Replaces Debian branding with TenebraOS in Calamares
│   │   └── 0050-boot-branding.hook.chroot # Removes Debian plymouth themes, uses generic spinner
│   └── 9000-tenebraos-modules.hook.chroot # Copies custom Calamares modules + settings
│
├── bootloaders/             # Custom GRUB menu entry templates (override live-build defaults)
│   ├── grub-pc/grub.cfg     #   BIOS boot: kernel cmdline includes boot=live
│   └── grub-efi/grub.cfg    #   EFI boot: same
│
├── includes.chroot/         # Files copied verbatim into the live system's root
│   ├── etc/
│   │   ├── calamares/settings.conf          # Calamares pipeline (welcome → usecase_select → ... → autoconfig)
│   │   └── sddm.conf.d/autologin.conf       # SDDM auto-logs in 'user' to Plasma
│   ├── usr/
│   │   ├── local/bin/tenebraos-installer.sh   # Launches Calamares via pkexec on live boot
│   │   ├── share/
│   │   │   ├── applications/calamares.desktop # Menu entry: "Install TenebraOS"
│   │   │   ├── calamares/set-wallpaper.sh     # Sets TenebraOS wallpaper on first login
│   │   │   ├── backgrounds/everydaylinuxuser/ # Desktop backgrounds (many wallpapers)
│   │   │   ├── wallpapers/tenebraos/          # KDE wallpaper
│   │   │   └── tenebraos/t2/t2-audio.conf     # T2 audio config
│   │   └── KDE/wallpapers/
│   └── tenebraos-src/                        # All custom TenebraOS source files
│       ├── installer/
│       │   ├── calamares/
│       │   │   ├── branding/tenebraos/        # Calamares brand: logo, splash, slideshow
│       │   │   ├── qml/calamares/slideshow/   # QML slideshow UI for installer
│       │   │   └── settings.conf              # Fallback Calamares settings
│       │   └── modules/                       # Custom Calamares modules
│       │       ├── hardware_detect/           #   Detects GPU vendor + Mac hardware
│       │       ├── usecase_select/            #   PyQt5 view: Gaming / Learning / Office picker
│       │       └── autoconfig/                #   Reads choice, runs profile script in chroot
│       └── profiles/                          # Post-install profile scripts
│           ├── drivers.sh                     #   install_brave() shared function
│           ├── gaming.sh                      #   Steam, Lutris, Discord, Wine, mangohud
│           ├── learning.sh                    #   VS Code, Python, Node.js, Jupyter, zram
│           └── office.sh                      #   LibreOffice, Thunderbird, TLP power saving
│
└── includes.binary/          # Files copied into the ISO binary (not live root)
    └── boot/grub/
        ├── config.cfg        # GRUB theme + hidden timeout
        └── themes/tenebraos/ # GRUB boot menu theme (background, colors, fonts)
```

## How the Calamares installer pipeline works

The custom installer flows through these Calamares modules:

```
[welcome] → [usecase_select] → [locale] → [keyboard] → [partition] → [users] → [summary]

  ↓ (user clicks Install)
  
[partition] → [users] → [networkcfg] → [grubcfg] → [bootloader] → [hwclock]
→ [services] → [packages] → [autoconfig] → [umount] → [finished]
```

The `hardware_detect` module runs silently before the sequence starts, detecting GPU vendor (NVIDIA/AMD/Intel) and whether the system is a Mac. This data is stored in Calamares' `globalStorage`.

The `usecase_select` module shows a PyQt5 screen asking "What will you use this system for?" with three options: Gaming, Learning & Development, Daily Use & Office. The choice is stored in `globalStorage`.

The `autoconfig` module runs at the end of installation. It reads the user's choice, copies the appropriate profile script (`gaming.sh`, `learning.sh`, or `office.sh`) into the installed system's `/tmp/`, and executes it via `chroot`. This installs all the selected apps (Steam, Discord, VS Code, etc.) and configures system settings.

## How to build

### Prerequisites

- **Debian 13 (Trixie)** or Debian testing (recommended for building)
- Packages: `live-build debootstrap squashfs-tools xorriso`
- For QEMU testing: `qemu-system-x86 ovmf`

### Build

```bash
sudo ./build.sh build      # Build the ISO (~20-40 min)
sudo ./build.sh test-qemu  # Boot in QEMU with UEFI
sudo ./build.sh flash      # Write ISO to USB
```

Or manually:

```bash
sudo lb clean --purge
sudo lb config \
    --apt-recommends true \
    --architecture amd64 \
    --archive-areas "main contrib non-free non-free-firmware" \
    --bootappend-live "components quiet splash" \
    --debian-installer false \
    --distribution trixie \
    --linux-flavours amd64 \
    --mode debian
sudo lb build
```

The ISO will be at `live-image-amd64.hybrid.iso`.

### Speed up builds

Install `apt-cacher-ng` to cache packages between rebuilds — this avoids re-downloading the entire package set each time:

```bash
sudo apt install apt-cacher-ng
echo 'LB_APT_HTTP_PROXY="http://localhost:3142"' | sudo tee -a config/common
```

## Custom kernel

To build with a custom kernel:

1. Build kernel `.deb` packages on a Debian system
2. Copy them to `config/packages.chroot/`
3. Rebuild — `lb build` will use your packages instead of Debian's

## FAQ

**Why does it boot to initramfs?**  
The `boot=live` kernel parameter must be present. It's hardcoded in `config/bootloaders/grub-pc/grub.cfg` and `grub-efi/grub.cfg`.

**Why is the display manager not starting?**  
Ensure `sddm` and `plasma-desktop` are in your package list. live-build installs without recommends by default, so meta-packages like `kde-plasma-desktop` won't pull in SDDM/KWin unless you either:
- Enable `--apt-recommends true` (done in build.sh), or
- List the dependencies explicitly (done in `live.list.chroot`)

**Why does Calamares ask for root?**  
Calamares needs root for partitioning. The launcher at `/usr/local/bin/tenebraos-installer.sh` uses `pkexec calamares`.

**Why does the ISO boot to CLI?**  
Check `config/package-lists/live.list.chroot` has `sddm` and `plasma-desktop`. Also verify `/etc/sddm.conf.d/autologin.conf` has `User=user` and `Session=plasma`.
