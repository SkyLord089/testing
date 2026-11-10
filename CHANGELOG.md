# Changelog

## 0.2.0 — CAD-first rebuild

- Replaced the mesh-first screen with a CAD-first workflow and a redesigned topology workspace.
- Added local STEP/STP B-REP parsing and adaptive surface tessellation with a chord-error control and quality presets.
- Added STEP circle-radius based tolerance adjustment: full circular curves are sampled with segment counts divisible by four, and larger radii receive more samples at equal error.
- Added planar connected-patch consolidation for OBJ output, wireframe inspection, interactive preview, and OBJ/STL export.
- Kept OBJ/STL as direct mesh inputs, with clear indication that triangulated formats no longer contain source CAD surfaces.

### Scope and limitations

- STEP assembly placements and vendor-specific AP242 extensions still need a wider fixture suite.
- Circle-count alignment targets complete circular STEP curves. Partial arcs are not a complete loop and are not subject to the same count rule.
- This is adaptive surface tessellation, not a production-grade quad-remesher or feature-aware edge-collapse optimizer. Mesh-only OBJ/STL input is not CAD-regenerated.
- Always validate the output in the target CAD/DCC/CAM application before production use.
