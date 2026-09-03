# TenebraOS apt repository
# ========================
# Free package hosting using a single GitHub Release as the apt server.

How it works
------------
Everything lives on one GitHub Release tag:

    https://github.com/iTzR1g/TenebraOS/releases/download/tenebraos-repo/
    ├── dists/tenebraos/InRelease          signed index (apt fetches this)
    ├── dists/tenebraos/Release
    ├── dists/tenebraos/Release.gpg
    ├── dists/tenebraos/tenebraos-repo.gpg public key
    ├── dists/tenebraos/main/binary-amd64/
    │   └── Packages                       package index (Filename = relative path)
    └── pool/*.deb                         binary packages

apt source line (on targets):
    deb [signed-by=/usr/share/keyrings/tenebraos-repo.gpg] \
        https://github.com/iTzR1g/TenebraOS/releases/download/tenebraos-repo/ \
        tenebraos main

apt constructs download URLs as: <base>/pool/<name>.deb
Index files (dists/) are also uploaded as release assets.

Packages
--------
  tenebraos-fastfetch  fastfetch with TenebraOS logo + default preset
  linux-t2             Apple T2-patched kernel (t2linux pre-built debs)
  linux-cachyos-t2     Custom kernel: T2 + CachyOS BORE scheduler + performance

Publishing a package
--------------------
    1.  ./repo/mirror-devuan.sh           mirror stock Devuan packages to pool/
    2.  ./repo/pkgs/<pkg>/build.sh        build/stage .deb into repo/pool/
    3.  ./repo/publish-repo.sh            generate + sign index
    4.  ./repo/upload-pool.sh             upload everything to GitHub Release
        OR: ./repo/publish-all.sh         all-in-one

pool/ and dists/ are gitignored — everything is a release asset under tag
"tenebraos-repo". The signing secret key lives at
~/.config/tenebraos/tenebraos-repo.asc (never commit it).

Rotate signing key:
    rm ~/.config/tenebraos/tenebraos-repo.asc && ./repo/publish-repo.sh
