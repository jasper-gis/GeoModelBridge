"""Synchronous, standard-library-only client for the GeoModelBridge executable.

Importing this package does not import ArcPy, load the FileGDB SDK, or start a process.
"""

from .client import (
    CallbackError, ConversionError, ConversionRequest, ConversionResult, Diagnostic, Engine,
    GeoModelBridgeError, Message, ProbeResult, ValidationError,
    InspectionBounds, InspectionCounts, InspectionError, InspectionRequest, InspectionResult,
)
from ._version import __version__

__all__ = [
    "CallbackError", "ConversionError", "ConversionRequest", "ConversionResult", "Diagnostic",
    "Engine", "GeoModelBridgeError", "Message", "ProbeResult", "ValidationError",
    "InspectionBounds", "InspectionCounts", "InspectionError", "InspectionRequest", "InspectionResult",
    "__version__",
]
