# Third-party notices / provenance

This implementation is offered under GPL-3.0-only; see LICENSE. It is an independent experimental module and is not an official KDE, Kdenlive or PaddleSeg release. Source is included. No model weights or third-party runtime binaries are bundled in the repository.

External projects referenced or required when building optional components:

| Project | Role | Upstream license/source to retain with actual distribution |
|---|---|---|
| zlib | mask frame compression / PNG dependency | zlib license; https://zlib.net/zlib_license.html |
| libpng | PNG I/O | libpng license; https://libpng.org/pub/png/src/libpng-LICENSE.txt |
| OpenSSL | SHA256 implementation | actual pinned distribution license; OpenSSL 3 uses Apache-2.0; https://github.com/openssl/openssl/blob/master/LICENSE.txt |
| MLT | editor render plugin API | license per included upstream component; https://github.com/mltframework/mlt |
| ONNX Runtime 1.21.0 | native CPU inference | MIT; https://github.com/microsoft/onnxruntime/blob/main/LICENSE |
| PaddleSeg PP-HumanSegV2-Lite | portrait human segmentation model; weights are not in this repository | Apache-2.0; https://github.com/PaddlePaddle/PaddleSeg/blob/release/2.10/LICENSE |

Obtain the complete license and notices of every actual pinned dependency/model before distributing a binary package. A reference to a license here does not replace the upstream license file and required notices. Do not remove original author attribution from any dependency added later.
