# Changelog

## 0.2.4 — bounded STEP tessellation resources
- Limit Truck/Rayon tessellation to at most two workers with larger worker stacks to reduce memory spikes on complex STEP assemblies.
- Use the faster parameter-based tessellation first and invoke robust nearest-point fallback only for faces it cannot mesh.

## 0.2.3 — clearer STEP failure state
- Hide the empty-workspace prompt as soon as a STEP file is selected.
- Replace the misleading “launch the executable” warning with distinct local-server disconnect and HTTP 404 diagnostics.
- Keep the selected STEP in the left panel with a retry status if tessellation does not return a mesh.
- Add `/api/health` version reporting and return a useful HTTP error if the STEP tessellator panics instead of dropping the connection.

## 0.2.2 — STEP parser compatibility
- Accept valid empty aggregates used by STEP exports from HOOPS Exchange / Plasticity through the upstream ruststep parser fix.
- Add a regression test for the uploaded AP203 file and replace the misleading AP203/AP214 error hint.

## 0.2.1 — STEP upload feedback fix
- Fixed a JavaScript function naming mismatch that caused the file-selection handler to fail before rendering the imported model.
- Confirmed the upload flow in a simulated browser page with the bundled UI scripts and `cube.step` fixture.

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
