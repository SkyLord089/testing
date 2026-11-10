# FacetForge

**FacetForge** is a small, local-first mesh cleanup and density-control tool for triangulated CAD exports. It includes a dependency-free Rust CLI and a browser UI (no build step or upload service). Models are processed locally.

> This first release is a practical mesh-cleanup prototype, not a full CAD-aware remesher. It welds nearby vertices, removes degenerate/duplicate triangles, and retains more samples around bends. It does not recover exact CAD surfaces or guarantee that detected circular loops have a vertex count divisible by four. Results depend on the input tessellation exported from Plasticity; inspect the result before manufacturing or production use.

## Browser app

Open `index.html` in a modern browser, or serve this directory with any static HTTP server. Import OBJ or STL, adjust **Density** and **Preserve detail**, inspect the mesh with drag-to-orbit and wheel-to-zoom, and export the processed mesh as triangulated OBJ. The demo model works without an upload. Files stay on your device.

## Rust CLI

Rust 2021, standard library only:

```sh
cargo run --release -- input.stl output.obj --density 72 --curve-detail 1.2
```

OBJ and binary/ASCII STL are accepted. Output is triangulated OBJ. `--density` ranges from 5 (more vertex merging) to 100 (keep source sampling); `--curve-detail` ranges from 0.5 (merge more aggressively) to 2 (merge less). For example:

```sh
cargo test
cargo run --release -- chair.obj chair-clean.obj --density 85
```

## Build

```sh
cargo build --release
```

Prebuilt CLI binaries for Linux, macOS, and Windows are attached to tagged GitHub releases by the release workflow. See [CHANGELOG](CHANGELOG.md) for scope and known limitations.
