# Project-authored FBX acceptance assets

These small ASCII FBX 7.4 fixtures and the generated 2 x 2 RGBA PNG were authored
for GeoModelBridge. The project owner has not selected an open-source license. They do not copy
third-party models. `checker.png` pixels (top row then bottom row): red, green,
blue, white. All pixels are opaque.

* `colored_quad.fbx`: one Z-up, meter quad spanning X=[0,2], Y=[0,3], Z=0.
  Two triangles after triangulation; Lambert RGBA=(0.4,0.1,0.05,0.75).
* `textured_quad.fbx`: same quad, white material, external `checker.png`.
  Two UV sets: first is constant zero; texture selects second `UV_Main`, [0,1]^2.
* `embedded_quad.fbx`: same appearance, PNG stored in Video Content as base64;
  the external filename deliberately does not exist.
* `missing_texture.fbx`: requests `does-not-exist.png`, must report MISSING_TEXTURE.
* `project_rgb.jpg`: the same project-generated 3×2 solid RGB JPEG already used
  by both writer integration suites. Reader regressions derive JFIF/Adobe/EXIF
  container variants from these bytes in a temporary directory. No private or
  external image is used; JPEG normalization must preserve all original bytes
  after the inserted JFIF segment and leave the source file untouched.
* `uv_transform.fbx`: UV texture-to-UV translation=(0.25,0.5), scaling=(2,3);
  baked sampling coordinates are ((u-0.25)/2, (v-0.5)/3).
* `instanced_mirror.fbx`: one geometry, two node instances, distinct materials.
  First inherits parent translation (100,200,300), node translation (10,20,30),
  scale (-2,3,1) and geometric translation (1,0,0). Bounds X=[104,108],
  Y=[220,229], Z=330, red material. Second bounds X=[0,2], Y=[0,3], Z=5,
  blue material. Winding and normals must agree after mirrored transformation.
* `unsupported_emission.fbx`: active emission must produce an error diagnostic.

They exercise the real parser and reader; synthetic in-memory scene fixtures
are separate tests. They are not a substitute for testing exporter-produced
binary FBX or ArcGIS/GeoScene visual and GDB roundtrip acceptance.

* `multi_material.fbx`: one quad as two explicit triangles; first red RGBA=(0.4,0.1,0.05,0.75), second opaque blue.
* `no_material.fbx`: the quad with no material assignment; explicit white default and DEFAULT_MATERIAL_ASSIGNED warning.
* `sloped_normals.fbx`: triangle (0,0,0),(1,0,0),(0,1,1), scaled by (2,3,4); transformed normals must equal (0,-0.8,0.6).

Exporter-produced binary fixtures are separately attributed under `upstream/`; see its README and original license.

`../profile_integration_test.py` additionally derives synthetic ASCII models
from this project-authored quad in a fresh temporary directory. It exercises
neutral exporter properties, empty versus active animation, saved static node
poses, explicit lighting-channel omission, unsupported displacement/opacity/
normal channels, missing images, and exact-zero versus tiny nonzero triangle
areas. No private user model is included in these regressions.
