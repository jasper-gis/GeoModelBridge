"""Synchronous, standard-library-only client for the GeoModelBridge executable.

Importing this package does not import ArcPy, load the FileGDB SDK, or start a process.
"""

from .client import (
    ConversionError, ConversionRequest, ConversionResult, Diagnostic, Engine,
    GeoModelBridgeError, Message, ProbeResult, ValidationError,
)
from ._version import __version__

__all__ = [
    "ConversionError", "ConversionRequest", "ConversionResult", "Diagnostic",
    "Engine", "GeoModelBridgeError", "Message", "ProbeResult", "ValidationError",
    "__version__",
]
