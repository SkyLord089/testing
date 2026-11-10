# FacetForge

FacetForge is a local CAD-to-mesh workbench. The preferred input is **STEP/STP**, because it retains analytic surfaces; OBJ/STL are also accepted as already-triangulated mesh inputs. The Rust application embeds a browser UI and serves it only on `127.0.0.1`. Source files are processed locally and are not uploaded.

## What is implemented

- STEP B-REP parsing and adaptive CAD-surface tessellation in Rust using the `truck` CAD kernel.
- A chord-error tolerance with quick, balanced, and fine presets. At a fixed absolute tolerance, larger-radius curved surfaces are sampled more densely while planar regions do not need curve-driven refinement.
- Interactive orbit/zoom preview, wireframe inspection, planar-region consolidation in OBJ export, and OBJ or STL mesh export.
- Direct OBJ/STL preview and export. Those formats no longer contain the original CAD surfaces, so the app cannot adaptively rebuild their topology from exact geometry.

## Run the app

Download the archive for your OS from [GitHub Releases](https://github.com/SkyLord089/testing/releases). Extract it, then run `facetforge` (Windows: `facetforge.exe`). With no command-line arguments it starts the embedded local UI and opens your browser. Keep the launcher window open while using the app.

To import a Plasticity model, export it as STEP/STP and open that file in the app. Output is a mesh in OBJ or STL format.

## CLI

Rust 2021:

```sh
cargo run --release -- chair.step chair.obj --tolerance 0.08
cargo run --release -- scan.stl cleaned.obj --density 85
cargo test
```

For STEP, `--tolerance` is the maximum chord deviation in the model's coordinate units. For an already tessellated OBJ/STL, `--density` controls conservative vertex clustering and cleanup; it cannot reconstruct exact CAD surfaces.

## Known limitations

This is a CAD-aware tessellator, not yet the full topology optimizer requested. Complete circular STEP curves are aligned to segment counts divisible by four; this does not apply to partial arcs. The tool does not yet guarantee an exact target polygon count or perform production-grade feature-aware edge collapse. Some STEP assembly/vendor extensions still need testing. See [CHANGELOG](CHANGELOG.md).
