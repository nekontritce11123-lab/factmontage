# External dependencies and attribution

This source package contains newly authored coordinator/UI/test code. It does
not contain FFmpeg, Qt, MLT, RNNoise or DeepFilterNet libraries/model weights,
and is not an official KDE or Kdenlive release.

FFmpeg — https://ffmpeg.org/ — external executable. Its applicable license
and notices depend on the exact build configuration and enabled components.
Qt — https://www.qt.io/ — optional panel dependency; retain the notices and
satisfy the terms of the exact Qt distribution used by the host project.
MLT — https://www.mltframework.org/ — the existing host's media framework.
RNNoise — https://github.com/xiph/rnnoise — optional underlying speech denoiser.
werman/noise-suppression-for-voice — GPL-3.0 plugin wrapper, not bundled here:
https://github.com/werman/noise-suppression-for-voice
DeepFilterNet is discussed as a candidate only; it is neither bundled nor used.

The MIT license for this package does not relicense any of those dependencies
or the user's Kdenlive distribution. Preserve the existing host project's
license notices and the notices of any libraries actually shipped with it.
