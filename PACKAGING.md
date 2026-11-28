# Packages for the zynthian package store

Each Eka plugin can be installed from the zynthian package store (webconf → Software → Packages), without building
anything on the device. The store is the [zynthian-packages](https://github.com/zynthian/zynthian-packages) catalog:
it holds only the metadata and an install script per package. The plugins themselves are downloaded from the
GitHub releases of this repository.

## What is in this repository

| Path | What it does |
| --- | --- |
| `.github/workflows/build.yml` | GitHub CI: on every push and pull request, builds all the plugins for x86_64 and arm64 inside Debian trixie (the zynthian OS base), checks each bundle with lilv and keeps the archives as workflow artifacts. A push to `main` publishes them in the rolling `latest` prerelease; a pushed version tag (`0.1`, `v0.2`…) publishes them in a release with that name. |
| `tools/package.sh` | Builds plugins and packages each one as a self-contained bundle: `dist/lv2/eka-<plugin>.lv2` (with its zynthian widget declared as a `zyngui:widget` UI, its licenses and, for Mist and Pads, its factory samples) and `dist/eka-<plugin>-lv2-<arch>.tar.gz`. The CI runs it; it works the same on any Linux with the build dependencies. |
| `tools/package_version.py` | Writes the store entries for a release into `package/Plugins/<Package>/` (`info.yml`, `<Package>.sh`, `Art/`), with descriptions taken from the plugins' sections of `README.md`. |
| `package/Plugins/` | The store entries, ready to copy into zynthian-packages: one package per plugin (`Eka Bloom`, `Eka Tape`…). |

arm64 binaries are built with `-mcpu=cortex-a72` (variable `EKA_AARCH64_CPU` of the build scripts), so they run on
the Raspberry Pi 4 and Pi 5. A build on the device with `make install` still uses `-mcpu=cortex-a76`.

### What a package installs

The package script uses the catalog's library (`zynpackage_lib.sh`, type `lv2_plugin`):

- downloads `eka-<plugin>-lv2-<arch>.tar.gz` from the release `VERSION` and unpacks it into
  `/zynthian/zynthian-plugins/lv2`, then enables the plugin in the engine list;
- on zynthian versions that load widgets from `zyngui/widgets_user` (Vangelis), links the widget there; newer
  versions find it in the bundle (`zyngui:widget`);
- Mist and Pads: links the factory samples as `zynthian-my-data/files/Samples/eka-<plugin>` and creates
  `eka-<plugin>-user` for the user's samples (the factory presets use those paths);
- creates the user presets bank, as `make install` does, and refreshes the plugin's preset list.

Uninstall removes the bundle and the links; the user's samples and presets are kept.

Don't install the same plugin both ways: a plugin installed with `make install` (in `/usr/local/lib/lv2`) and from
the store would be listed twice. Run `sudo make uninstall-<plugin>` before installing it from the store.

## Publishing a release in the store

1. **Set the version** of the store entries and commit them (the CI refuses a tag that doesn't match `package/`):

   ```bash
   tools/package.sh                  # optional: build here so that info.yml gets the measured sizes
   tools/package_version.py 0.2      # VERSION="0.2" in the scripts, version: "0.2" in info.yml
   git add package && git commit -m "Packages: 0.2"
   git push
   ```

   webconf offers an update when the `version` in the catalog is greater than the installed one (compared as
   text), so keep versions that sort the same way as text and as numbers: `0.2` … `0.9`, then `1.0`.

2. **Tag the release** and push the tag:

   ```bash
   git tag 0.2
   git push origin 0.2
   ```

   The CI builds both architectures and creates the GitHub release `0.2` with the 30 archives
   (`eka-<plugin>-lv2-x86_64.tar.gz` and `eka-<plugin>-lv2-arm64.tar.gz`). Wait for it to finish
   (Actions tab) and check that the release has all of them before going on: the packages download from it.

3. **Send the pull request to zynthian-packages.** Fork
   [zynthian/zynthian-packages](https://github.com/zynthian/zynthian-packages) on GitHub (once), then:

   ```bash
   git clone https://github.com/<you>/zynthian-packages.git
   cd zynthian-packages
   git remote add upstream https://github.com/zynthian/zynthian-packages.git
   git fetch upstream && git checkout -b eka-0.2 upstream/main
   cp -r ../zynthian-eka/package/Plugins/* Plugins/
   git add Plugins && git commit -m "Eka plugins 0.2"
   git push -u origin eka-0.2
   ```

   Open the pull request from the `eka-0.2` branch of your fork to `zynthian/zynthian-packages:main` (GitHub shows a
   "Compare & pull request" button; with the GitHub CLI: `gh pr create --repo zynthian/zynthian-packages`). Say in
   the description which release it installs and on which zynthian you tested it. Don't edit `packages.json`: the
   catalog's CI regenerates it after the merge.

   To test before the pull request is merged, on a zynthian (as root):

   ```bash
   cd /tmp && git clone --depth 1 https://github.com/zynthian/zynthian-packages.git catalog
   cp -r /path/to/zynthian-eka/package/Plugins/* catalog/Plugins/
   source /zynthian/config/zynthian_envars.sh
   bash "catalog/Plugins/Eka Bloom/Eka Bloom.sh" install    # also: installed, uninstall
   systemctl restart zynthian
   ```

   (The script sources `$ZYNTHIAN_DIR/zynthian-packages/zynpackage_lib.sh`, which is on every zynthian whose
   catalog is up to date.)

A new plugin gets its store entry the next time `tools/package_version.py` runs, as long as it has its
`### [Eka Name](plugins/<plugin>): …` section in `README.md` and, for the icon, a `docs/1-*.png` screenshot.
