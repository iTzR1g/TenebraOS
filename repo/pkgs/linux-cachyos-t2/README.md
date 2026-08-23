# TenebraOS Kernel: T2 + CachyOS Performance

Custom kernel combining Apple T2 Mac support with CachyOS performance patches, built inside a Debian Docker container from your Arch host.

## What's Included

**Apple T2 Support** (from t2linux):
- T2BCE driver stack (audio, DMA, virtual HCI)
- Apple SMC, HID, GPU multiplexer
- BCM4377 WiFi/Bluetooth
- APFS filesystem

**CachyOS Performance** (from CachyOS/kernel-patches):
- BORE scheduler (Burst-Oriented Response Enhancer)
- Multi-Gen LRU (MGLRU) memory management
- DAMON (Data Access Monitor) for proactive reclaim
- BBR TCP congestion control
- AMD P-State CPU frequency scaling

**Devuan Optimizations**:
- No systemd (cgroups v1, runit-compatible)
- 1000Hz tick rate, preemptible kernel

## Build

```bash
# Devuan/Debian — one-time deps (ISO + kernel + repo publishing):
sudo ./setup-build-deps.sh
sudo ./repo/pkgs/linux-cachyos-t2/build.sh

# Any other host (Arch etc.) — same script, runs in a debian:trixie container:
./repo/pkgs/linux-cachyos-t2/build.sh

# Pin version / scheduler:
KERNEL_VERSION=6.18 CACHYOS_SCHED=eevdf ./repo/pkgs/linux-cachyos-t2/build.sh
```

## Output

`.deb` packages appear in `repo/pool/`:
- `linux-image-<ver>-<rel>-t2-tenebra.deb`
- `linux-headers-<ver>-<rel>-t2-tenebra.deb`

## Install on T2 Mac

```bash
sudo dpkg -i linux-image-*.deb linux-headers-*.deb
sudo update-grub
sudo reboot
```

Auto-installed by Calamares when `is_mac_t2=true`.

## Scheduler Variants

| Variable | Description |
|----------|-------------|
| `bore` (default) | BORE — best desktop responsiveness |
| `eevdf` | Default Linux scheduler |
| `bmq` | Project C alternative |

## Patch Sources

- [t2linux/linux-t2-patches](https://github.com/t2linux/linux-t2-patches)
- [CachyOS/kernel-patches](https://github.com/CachyOS/kernel-patches)
