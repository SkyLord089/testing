# Changelog

## 0.2.0 — CAD-first rebuild

- Rebuilt the UI around native STEP import rather than mesh-first OBJ/STL upload.
- Added local STEP B-REP parsing and adaptive tessellation through the Rust `truck` CAD kernel, with a configurable chord-error tolerance.
- Added quality presets, interactive model preview, planar-patch consolidation for OBJ export, and OBJ/STL export.
- Added richer import errors and an explicit distinction between STEP CAD and already-triangulated mesh inputs.

### Scope that remains incomplete

- STEP assembly transforms and all vendor-specific AP242 extensions need broader fixture testing.
- This is adaptive surface tessellation, not a full remeshing/edge-collapse optimizer. Mesh-only OBJ/STL input can be previewed and exported but does not receive CAD-aware regeneration.
- Exact circular-loop segment counts divisible by four are not yet measured and enforced after tessellation. The chord-error tessellator naturally increases sampling with circle radius, but that is not a formal per-loop guarantee.
- Always inspect output in the target CAD/DCC/CAM application before production use.
