# Changelog

## 0.1.0 — Initial preview

- Local browser interface with OBJ/STL import, orbitable mesh preview, density/detail controls, statistics, and OBJ export.
- Dependency-free Rust CLI for OBJ and binary/ASCII STL input.
- Vertex welding, bent-area detail preservation, and degenerate/duplicate triangle cleanup.
- GitHub Actions checks and release builds for Linux, macOS, and Windows.

### Known limitations

- This is mesh cleanup and vertex clustering, not CAD-surface reconstruction, quad-dominant retopology, or a production-grade adaptive remesher.
- There is no circle/arc recognition, so the tool cannot promise circle edge counts divisible by four.
- Density is approximate and is not a requested exact triangle count. Always validate the result in your target CAD/DCC application.
