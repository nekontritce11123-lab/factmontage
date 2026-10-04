# Third-party notices

The original C++, JavaScript and Python code in this module is provided under MIT; see LICENSE.

The distributed Linux binary was compiled with GNU GCC 14 and statically links libstdc++ and libgcc. These runtime components are covered by their upstream GNU GPL terms and the GCC Runtime Library Exception where applicable. Upstream/Debian copyright and exception notices are reproduced in `licenses/GCC_DEBIAN_COPYRIGHT.txt`; GPL version 3 is in `licenses/GPL-3.txt`.

The C ABI is implemented from the public Frei0r interface. No Frei0r implementation, MLT implementation, Kdenlive source, GSAP library, Adobe source, paid animation template, font file, AI model or Qt library is copied into this module. The public documentation and creative techniques studied are listed in `docs/RESEARCH_RU.md`.

The native library dynamically uses the host/runtime libc and libm; these system libraries are not redistributed in the ZIP. Python and the browser are not redistributed. Optional demo/video utilities use externally installed Pillow and FFmpeg.

Example STXT scenes contain rasterized letters of the example phrases, not font files. Do not place fonts from a development machine into release archives. User-supplied font licensing remains the user's responsibility.
