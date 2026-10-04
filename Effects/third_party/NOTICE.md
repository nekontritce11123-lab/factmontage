# Frei0r API provenance

The offline `frei0r.h` contains the declarations from the official Frei0r 1.2
header, with documentation comments removed. It was reconstructed from the
upstream source returned by the GitHub connector, not downloaded as an untouched
source file. Therefore the offline C-host test is explicitly not the requested
"independent host built with an untouched official header" release gate.

Official upstream: https://github.com/dyne/frei0r/blob/master/include/frei0r.h
Observed Git blob: `b20487e9d34de76c7665b59378940317f63db2b4`.

`../../scripts/check_official_header.sh` downloads that exact Git blob, verifies
its Git object hash and writes the **unmodified** header. The SDK build compiles
both the filter and independent test host against this version. It has not been
executed in the current offline container.

The independent FFmpeg host installed in the current environment has separately
loaded and exercised the real plugin ABI, including all ten explicit numerical
parameters, RGBA channel order and partially transparent input. That test does
not depend on this offline header transcription.

Original effects implementation and generated assets: GPL-3.0-or-later, see
root LICENSE. No external effect implementation has been copied into the engine.
