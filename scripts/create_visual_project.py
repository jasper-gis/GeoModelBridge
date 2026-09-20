"""Create reproducible ArcGIS Pro scenes and rendered PNGs without changing source GDBs.

Run with the licensed ArcGIS Pro Python environment (tested with Pro 3.6.2).
Uses an installed Esri Blank.aprx or an explicitly supplied empty template.
Output directories must be new; this script never overwrites a project.
"""
import argparse
import json
import math
from pathlib import Path

import arcpy


def rgb(values):
    color = arcpy.cim.CreateCIMObjectFromClassName("CIMRGBColor", "V3")
    color.values = values
    return color


def camera_for(frame, bounds, view):
    xmin, ymin, zmin, xmax, ymax, zmax = bounds
    cx, cy, cz = (xmin + xmax) / 2, (ymin + ymax) / 2, (zmin + zmax) / 2
    size = max(xmax - xmin, ymax - ymin, zmax - zmin, 1.0)
    camera = frame.camera
    camera.roll = 0
    if view == "top":
        camera.X, camera.Y, camera.Z = cx, cy, cz + size * 3.0
        camera.heading, camera.pitch = 0, -90
    else:
        # Position south-east of the feature, looking north-west and down.
        distance = size * 4.0
        pitch = 35.264389682754654 if view == "opposite" else -35.264389682754654
        heading = 135 if view == "opposite" else -45
        horizontal = distance * math.cos(math.radians(pitch))
        camera.X = cx - horizontal * math.sin(math.radians(heading))
        camera.Y = cy - horizontal * math.cos(math.radians(heading))
        camera.Z = cz - distance * math.sin(math.radians(pitch))
        camera.heading, camera.pitch = heading, pitch
    return {key: getattr(camera, key) for key in ("X", "Y", "Z", "heading", "pitch", "roll", "mode")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--examples", type=Path, help="Reference examples directory with filegdb/ and standalone/")
    parser.add_argument("--output", required=True, type=Path, help="New output directory")
    parser.add_argument("--render-output", type=Path, help="New PNG directory, default OUTPUT/renders")
    parser.add_argument("--template", type=Path, help="An empty APRX template; default is Esri's installed Blank.aprx")
    parser.add_argument("--no-export", action="store_true", help="Create APRX and fixed bookmarks without PNG export")
    parser.add_argument("--projection", choices=["Perspective", "Isometric"], default="Perspective", help="3D scene drawing mode")
    parser.add_argument("--extra-gdb", action="append", default=[], help="Additional multipatch GDB, NAME=PATH (Models feature class)")
    parser.add_argument("--only", nargs="*", choices=["color-cube", "uv-plane", "mixed-materials", "alpha-plane", "seam-cube", "standalone-copy"])
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    version = "V" + (root / "VERSION").read_text(encoding="utf-8").strip().lstrip("Vv")
    examples = (args.examples or root / "examples" / version).resolve()
    template = args.template or Path(arcpy.GetInstallInfo()["InstallDir"]) / "Resources/ArcToolBox/Services/routingservices/data/Blank.aprx"
    if not template.is_file():
        raise FileNotFoundError("No installed Blank.aprx. Create an empty ArcGIS Pro project and supply --template.")
    cases = [(name, examples / "filegdb" / (name + ".gdb")) for name in
             ("color-cube", "uv-plane", "mixed-materials", "alpha-plane", "seam-cube")]
    cases.append(("standalone-copy", examples / "standalone/alpha-plane-copy.gdb"))
    if args.only:
        cases = [case for case in cases if case[0] in args.only]
    for extra in args.extra_gdb:
        name, separator, path = extra.partition("=")
        if not separator or not name or any(c in name for c in '\\/:*?"<>|'):
            parser.error("--extra-gdb must be NAME=PATH with a valid filename-style NAME")
        cases.append((name, Path(path).resolve()))
    if len({name for name, _ in cases}) != len(cases):
        parser.error("Each case name must be unique")
    for name, gdb in cases:
        if not arcpy.Exists(str(gdb / "Models")):
            raise FileNotFoundError(str(gdb / "Models"))
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    render_out = args.render_output.resolve() if args.render_output else out / "renders"
    render_out.mkdir(parents=True, exist_ok=False)
    (out / "maps").mkdir()
    project = arcpy.mp.ArcGISProject(str(template))
    for layout in project.listLayouts():
        project.deleteItem(layout)
    for existing in project.listMaps():
        project.deleteItem(existing)
    project.homeFolder = str(out)
    project.updateFolderConnections([{"connectionString": str(out), "alias": "Visual acceptance", "isHomeFolder": True}])
    arcpy.management.CreateFileGDB(str(out), "visual-workspace.gdb")
    project.defaultGeodatabase = str(out / "visual-workspace.gdb")
    project_path = out / ("GeoModelBridge-visual-" + version + ".aprx")
    summary = {"version": version, "arcgis_pro_version": arcpy.GetInstallInfo()["Version"],
               "template": str(template), "source_examples": str(examples), "project": str(project_path),
               "status": "prepared_not_visually_accepted", "projection": args.projection, "cases": []}
    for index, (name, gdb) in enumerate(cases, 1):
        print("Preparing " + name, flush=True)
        scene = project.createMap(f"{index:02d} {name}", "SCENE")
        for layer in scene.listLayers():
            scene.removeLayer(layer)
        scene.spatialReference = arcpy.SpatialReference(32650)
        cim = scene.getDefinition("V3")
        cim.defaultViewingMode = "SceneLocal"
        cim.backgroundColor = rgb([235, 240, 246, 100])
        # Removing the ground reference removes terrain and its network source references.
        # These are scene display settings, not changes to feature coordinates.
        cim.groundElevationSurfaceLayer = ""
        cim.customElevationSurfaceLayers = []
        cim.enableNavigationBelowGround = True
        cim.illumination.showStars = False
        cim.illumination.enableAmbientOcclusion = False
        cim.illumination.enableEyeDomeLighting = False
        scene.setDefinition(cim)
        layer = scene.addDataFromPath(str(gdb / "Models"))
        layer.name = name + " | embedded materials"
        layer.elevation.setElevationMode("ABSOLUTE_HEIGHT", "Shape.Z")
        layer.elevation.setVerticalUnits("METERS")
        layer.elevation.verticalExaggeration = 1
        layer.elevation.cartographicOffset = 0
        layer_cim = layer.getDefinition("V3")
        symbol = layer_cim.renderer.symbol.symbol
        if type(symbol).__name__ != "CIMMeshSymbol":
            raise RuntimeError("Multipatch layer did not receive a CIMMeshSymbol")
        for material in symbol.symbolLayers:
            if type(material).__name__ == "CIMMaterialSymbolLayer":
                # White Multiply is identity, retaining feature-owned colors and textures.
                material.color = rgb([255, 255, 255, 100])
                material.materialMode = "Multiply"
        layer.setDefinition(layer_cim)
        # Feature-class metadata may omit Z extrema; geometry extents include them.
        with arcpy.da.SearchCursor(str(gdb / "Models"), ["SHAPE@"]) as cursor:
            extents = [row[0].extent for row in cursor]
        bounds = [min(getattr(e, key) for e in extents) for key in ("XMin", "YMin", "ZMin")]
        bounds += [max(getattr(e, key) for e in extents) for key in ("XMax", "YMax", "ZMax")]
        if not all(math.isfinite(number) for number in bounds):
            raise RuntimeError(f"Invalid 3D bounds for {name}: {bounds}")
        views = ["top"] if "plane" in name or name == "standalone-copy" else ["oblique", "top"]
        if name in ("color-cube", "seam-cube"):
            views.insert(1, "opposite")
        record = {"name": name, "source_gdb": str(gdb), "bounds": bounds,
                  "elevation": layer.elevation.elevationMode, "is_3d_layer": layer.is3DLayer, "views": []}
        for view_index, view in enumerate(views):
            layout = project.createLayout(8, 6, "INCH", f"{index:02d} {name} {view}")
            rectangle = arcpy.Polygon(arcpy.Array([arcpy.Point(0, 0), arcpy.Point(8, 0), arcpy.Point(8, 6), arcpy.Point(0, 6), arcpy.Point(0, 0)]))
            frame = layout.createMapFrame(rectangle, scene, name + " " + view)
            frame_cim = frame.getDefinition("V3")
            frame_cim.view.viewingMode = "SceneLocal"
            frame_cim.view.sceneDrawingMode = args.projection
            frame_cim.view.fieldOfView = 55
            frame_cim.useMapBackgroundColor = True
            frame.setDefinition(frame_cim)
            if frame.camera.mode != "LOCAL":
                raise RuntimeError("Map frame did not switch to a 3D local camera")
            camera = camera_for(frame, bounds, view)
            frame.createBookmark(name + " " + view, "Fixed inspection camera; true feature Z in metres")
            if view_index == 0:
                scene_cim = scene.getDefinition("V3")
                scene_cim.defaultCamera = frame.getDefinition("V3").view.camera
                scene.setDefinition(scene_cim)
            path = render_out / (name + "-" + view + ".png")
            if not args.no_export:
                print("Exporting " + path.name, flush=True)
                frame.exportToPNG(str(path), resolution=160, color_mode="24-BIT_TRUE_COLOR")
            record["views"].append({"view": view, "camera": camera, "render": str(path) if not args.no_export else None})
        scene.exportToMAPX(str(out / "maps" / (name + ".mapx")))
        record["elevation_surface_count"] = len(scene.listElevationSurfaces())
        record["layer_count"] = len(scene.listLayers())
        summary["cases"].append(record)
        project.saveACopy(str(project_path))
        (out / "visual-project.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")
    print("Prepared " + str(project_path), flush=True)


if __name__ == "__main__":
    main()
