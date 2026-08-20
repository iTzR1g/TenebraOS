# TenebraOS apt repository
# ========================
# Free package hosting using GitHub Releases as the file server and this git
# repo for the apt index. Same pattern as the t2linux project's repo.

How it works
------------
    repo/pool/*.deb                     binary packages (GitHub Release assets)
    repo/dists/tenebraos/main/binary-amd64/
        Packages                        index (Filename = release asset URL)
        Release, InRelease, Release.gpg signed metadata
    repo/dists/tenebraos/tenebraos-repo.gpg
                                        public key (shipped inside the ISO)

Targets use:
    deb [signed-by=/usr/share/keyrings/tenebraos-repo.gpg]
        https://github.com/iTzR1g/TenebraOS/releases/download/tenebraos-repo-pool/ ./

Packages
--------
  tenebraos-fastfetch  fastfetch with TenebraOS logo + default preset
  linux-image-*-t2 (-trixie)  Apple T2-patched kernel (t2linux builds), installed
                       only when the Calamares hardware_detect module finds a T2 Mac

Publishing a package
--------------------
   1.  ./repo/pkgs/<pkg>/build.sh        build/stage .deb into repo/pool/
       or: ./repo/publish-all.sh         everything at once (packages + index + upload)
   2.  ./repo/publish-repo.sh            regenerate + sign Packages/Release
                                         (generates the signing key on first run)
   3.  ./repo/upload-pool.sh             upload repo/pool/*.deb as release assets
                                         (requires gh CLI + GitHub auth: gh auth login)
   4.  git add repo/dists repo/pkgs && git commit && git push

pool/ is gitignored — .debs live only as GitHub Release assets under the tag
"tenebraos-repo-pool"; the committed index (Filename: -> release asset URLs) is
the repo. Don't forget the isotope: repo/dists/tenebraos/tenebraos-repo.gpg is
shipped inside the ISO and must match the key used by publish-repo.sh.

tenebraos-fastfetch is repacked from the upstream fastfetch release .deb
(repo/repack-deb.py injects the TenebraOS logo + preset — no toolchain needed).
linux-t2 simply stages the official t2linux trixie kernel debs.

The signing secret key lives at ~/.config/tenebraos/tenebraos-repo.asc on the
maintainer machine — never commit it. rotate it with:
    rm ~/.config/tenebraos/tenebraos-repo.asc && ./repo/publish-repo.sh