# Upstream binary FBX acceptance fixtures

These files are unchanged test assets from [ufbx](https://github.com/ufbx/ufbx),
tag `v0.23.0`, commit `fcc5d6ba444cfd3eb80677dba5e37e493941abe5`.
The repository's original dual MIT / public-domain-alternative license is
retained in `LICENSE`, including copyright attribution to Samuli Raivio.
No separate license was present under `data/` at that commit. The exact source
URLs, byte lengths, and SHA-256 digests are in `manifest.json`. The tests verify
these offline; running tests never downloads dependencies or models.

* `blender_293_half_smooth_cube_7400_binary.fbx` is a Blender-exported binary
  FBX 7.4 cube with UV seams, a mixture of smooth and hard normals, and no
  assigned material. Positive acceptance: one mesh, 12 triangles, 36 corner
  vertices, bounds approximately [-1,1] meters on each axis, right-handed
  Z-up, and an explicit white default material warning. Original per-corner
  normal and UV discontinuities survive canonicalization.
* `maya_cube_7400_binary.fbx` is a Maya-exported binary FBX 7.4 cube.
  In V0.1.3 both strict and static-GIS preparation accept its one mesh and
  12 triangles. Its animation stack has no active animation; its reflection
  and displacement-color properties are neutral defaults. The empty container
  produces an explicit warning. Geometry and material must be identical under
  both policies, with no compatibility adjustment. V0.1.2 falsely rejected
  these metadata/default properties; the original asset remains unchanged.

These add real exporter-produced binary parsing coverage. They do not prove
support for every FBX version, exporter, or material, and they do not validate
textured FileGDB delivery by themselves.
