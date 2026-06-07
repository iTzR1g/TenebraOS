# TenebraOS

**A Debian 13 (Trixie) based Linux distribution** with a graphical installer that auto-configures hardware, GPU drivers, and user profiles on first install.

---

## Features

- Auto-detects T2 MacBooks, legacy Macs, and standard PCs
- Auto-installs the correct GPU drivers (Nvidia, AMD, Intel)
- Three use-case profiles: **Gaming**, **Learning & Development**, **Daily Use & Office**
- Graphical installer built on **Calamares** (same as Ubuntu/Mint)
- Zero post-install setup required

---

## Trixie-specific notes

| Topic | Note |
|---|---|
| **Base** | Debian 13 (Trixie) — currently "testing", freezes before stable release |
| **GNOME** | Ships GNOME 48 |
| **KDE** | Ships Plasma 6 |
| **PipeWire** | Default audio system (replaces PulseAudio) |
| **T2 kernel** | t2linux repo targets stable; check wiki.t2linux.org for Trixie availability |
| **Package names** | If `lb build` fails on a missing package, check it hasn't been renamed in Trixie |

---

## Project Structure

```
TenebraOS/
├── build.sh                            ← Build & test helper
├── live-build/
│   ├── init-lb.sh                      ← Initialize lb config (run once)
│   └── config/
│       ├── package-lists/
│       │   └── base.list.chroot        ← Packages in every install
│       └── hooks/
│           └── 9000-tenebraos-modules.hook.chroot  ← Copies modules into live system
├── installer/
│   ├── calamares/
│   │   ├── settings.conf               ← Installer pipeline
│   │   └── branding/tenebraos/
│   │       └── branding.desc           ← Colors, logo, product name
│   └── modules/
│       ├── hardware_detect/            ← Python: Mac/GPU detection
│       ├── usecase_select/             ← Python: Profile selection UI
│       └── autoconfig/                 ← Shell: Post-install config
├── profiles/
│   ├── drivers.sh                      ← GPU & Mac driver functions
│   ├── gaming.sh                       ← Gaming profile (KDE Plasma 6)
│   ├── learning.sh                     ← Learning/Dev profile (GNOME 48)
│   └── office.sh                       ← Daily Use/Office profile (GNOME 48)
└── branding/
    ├── logo.png                        ← Add your logo here
    └── wallpaper.png                   ← Add your wallpaper here
```

---

## Quick Start

### 1. Install build tools on a Debian/Ubuntu host

```bash
sudo apt update
sudo apt install -y live-build calamares calamares-settings-debian \
    python3 python3-pip git debootstrap squashfs-tools xorriso \
    grub-efi-amd64-bin grub-pc-bin isolinux ovmf qemu-system-x86
```

### 2. Initialize live-build

```bash
./build.sh init
```

### 3. Build the ISO

```bash
./build.sh build
# Output: live-build/live-image-amd64.hybrid.iso
```

### 4. Test in QEMU

```bash
./build.sh test-qemu
```

### 5. Flash to USB

```bash
./build.sh flash
```

---

## Branding

Place your files in `installer/calamares/branding/tenebraos/`:

- `logo.png` — sidebar logo (recommended: 200×200px)
- `welcome.png` — welcome screen background

Colors are set in `branding.desc` — currently a deep dark navy/purple theme.

---

## Resources

- [live-build manual](https://live-team.pages.debian.net/live-manual/)
- [Calamares docs](https://calamares.io/docs/)
- [t2linux wiki](https://wiki.t2linux.org)
- [Debian Trixie release info](https://www.debian.org/releases/trixie/)
- [Linux Mint Calamares config (reference)](https://github.com/linuxmint/calamares-settings-mint)
