"""Plain Python example, no ArcPy or toolbox required. Use --help for arguments."""
import argparse
from pathlib import Path
import sys

# A full release has this example in <release>/python/examples.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from geomodelbridge import CallbackError, ConversionRequest, Engine, GeoModelBridgeError


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--wkid", type=int, required=True)
    parser.add_argument("--origin", type=float, nargs=3, required=True)
    parser.add_argument("--profile", choices=("strict", "gis-static"), default="strict")
    parser.add_argument("--missing-textures", choices=("material-color", "error"), default="material-color")
    args = parser.parse_args()
    try:
        result = Engine(args.engine).convert(ConversionRequest(
            args.input, args.output, args.wkid, tuple(args.origin),
            profile=args.profile, missing_textures=args.missing_textures,
        ), on_message=lambda message: print(message.text, flush=True))
        print(f"Feature class: {result.feature_class_path}\nReport: {result.report_path}")
    except GeoModelBridgeError as error:
        print(f"[{error.code}] {error}", file=sys.stderr)
        if isinstance(error, CallbackError) and error.result is not None:
            print(f"Conversion verified; only message delivery failed. Feature class: {error.result.feature_class_path}", file=sys.stderr)
        for diagnostic in error.diagnostics[:10]:
            print(f"[{diagnostic.code}] {diagnostic.message}", file=sys.stderr)
        if error.report_path:
            print(f"Report: {error.report_path}", file=sys.stderr)
        if error.stderr_tail:
            print(error.stderr_tail[-4000:], file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
