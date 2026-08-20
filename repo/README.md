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
  1.  ./repo/pkgs/<pkg>/build.sh        build .deb into repo/pool/
  2.  ./repo/publish-repo.sh            regenerate + sign Packages/Release
                                        (generates the signing key on first run)
  3.  ./repo/upload-pool.sh             upload repo/pool/*.deb as release assets
                                        (requires gh CLI + GitHub auth)
  4.  git add repo/ && git commit && git push

The signing secret key lives at ~/.config/tenebraos/tenebraos-repo.asc on the
maintainer machine — never commit it. rotate it with:
    rm ~/.config/tenebraos/tenebraos-repo.asc && ./repo/publish-repo.sh