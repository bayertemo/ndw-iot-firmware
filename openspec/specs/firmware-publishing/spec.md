# firmware-publishing Specification

## Purpose
How builds are published: the generated manifest the flasher reads, the one-chip-per-build rule, and the CI that gates and releases boards.

## Requirements

### Requirement: The manifest is generated and checked
The firmware SHALL work as follows. `scripts/build-manifest.py` generates `manifest.json` from `builds/**/build.json`, with each part's size and SHA-256. It fails on a `kind` that disagrees with its path, an invalid `chipMatch` regex, two parts at one address, duplicate kinds, or two builds of one role on the same chips and vendor.

#### Scenario: A kind that disagrees with its path
- **WHEN** a `build.json` names a different `kind` than its folder
- **THEN** the script fails and names the file
<!-- proved: scripts/build-manifest.py:69-73, :78-80, :137-140, :148-156 -->

### Requirement: A build without binaries is published as unavailable
The firmware SHALL work as follows. A build whose binaries are missing is listed with `"published": false`; a published path must resolve from the repository root.

#### Scenario: Missing binary
- **WHEN** a part's file is absent
- **THEN** the build is marked unpublished and the manifest still builds
<!-- proved: scripts/build-manifest.py:110-112, :176-191 -->

### Requirement: One build, one chip
The firmware SHALL work as follows. Each `build.json` targets one chip family by a `chipMatch` regex tested against what esptool-js reports, with a negative lookahead so `^ESP32-C3` does not match an ESP32-C61. Adding a chip adds a directory.

#### Scenario: An ESP32-C61
- **WHEN** the console connects to an ESP32-C61
- **THEN** the C3 build does not match
<!-- proved: builds/ndw/espressif/esp32/c3/bl/lorawan-fleet/build.json:7 (chipMatch with (?!\d)) -->

### Requirement: Host tests gate every board build
The firmware SHALL work as follows. CI runs the host tests before any board is built; a board built off `main` is committed with its binaries and the manifest, anywhere else it is a `-rc.<n>+sha` build attached to the run and never committed.

#### Scenario: A pull request
- **WHEN** CI runs on a branch
- **THEN** the build is versioned `-rc` and not published
<!-- proved: .github/workflows/build.yml:46-56 (test-native), :109 (rc version), :260 -->
