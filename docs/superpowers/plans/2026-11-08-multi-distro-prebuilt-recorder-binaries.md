# Multi-Distro Prebuilt Recorder Binaries Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

## Overview
Enable zero-compiler, out-of-the-box screen recording across diverse Linux distributions (Ubuntu/Debian family with FFmpeg 7 `.so.62`, and Arch/CachyOS/Fedora/openSUSE family with FFmpeg 8 `.so.63`) by packaging prebuilt recorder binary variants inside the release archive and making `install.sh` automatically select the compatible binary without requiring `clang` or `g++`.

## Proposed Architecture
1. **Multi-Variant Packaging (`scripts/package_release.sh` & `.github/workflows/ci.yml`)**:
   - Primary build: `build/fpshud_recorder_bin` (Ubuntu 24.04 baseline, `libavformat.so.62`).
   - Secondary build: `build/fpshud_recorder_bin-arch` (Arch Linux baseline, `libavformat.so.63`, built via standard container in CI).
   - Release archive includes both `fpshud_recorder_bin` and `fpshud_recorder_bin-arch` (~280 KB each).
2. **Dynamic Selection Engine in `install.sh`**:
   - Inspects host compatibility by running candidate binaries with `--help`.
   - Iterates through candidates:
     1. Default `fpshud_recorder_bin`
     2. Arch/Rolling `fpshud_recorder_bin-arch`
     3. Any additional `fpshud_recorder_bin-*` variants
   - The first candidate that exits without status 127 (`missing shared library`) is copied to `$LIB_DIR/fpshud_recorder_bin` and `$BIN_DIR/fpshud_recorder_bin`.
   - Fallback to native auto-compilation if neither matches and `clang++`/`g++` is installed.
   - Graceful notice if no candidate matches and no compiler is present.
3. **Automated Verification**:
   - Update `tests/test_cli.cpp` to verify multi-distro recorder selection and fallback logic.
   - Test locally on CachyOS (verifying that `fpshud_recorder_bin-arch` is selected directly without compiler invocation).

## Tasks

- [x] Task 1: Update `scripts/package_release.sh` to package all `fpshud_recorder_bin*` variants
  - File: `scripts/package_release.sh`
  - Ensure any matching `fpshud_recorder_bin*` in `$BUILD_DIR` is stripped and added to the tarball.

- [x] Task 2: Implement Multi-Variant Detection & Selection in `install.sh`
  - File: `install.sh`
  - Implement candidate search loop: `fpshud_recorder_bin`, `fpshud_recorder_bin-arch`, `fpshud_recorder_bin-*`.
  - Copy selected binary to target paths with chmod 755.

- [x] Task 3: Update CI Workflow to Build Multi-Distro Binary Variants
  - File: `.github/workflows/ci.yml`
  - In `build-test-release` job before packaging, build `build/fpshud_recorder_bin-arch` using Docker (`archlinux:latest`).
  - Ensure both binaries are packaged into `release_pkg/fpshud-linux-x86_64.tar.gz`.

- [x] Task 4: Unit Test Verification & Local Verification
  - Files: `tests/test_cli.cpp`
  - Run `./build/fpshud_test 1` to verify all 89 test suites pass.
  - Verify that `install.sh` correctly picks the Arch binary on the host without invoking a compiler.

- [ ] Task 5: Commit, Push, and Verify CI Trigger
  - Git commit and push changes to `main`.
