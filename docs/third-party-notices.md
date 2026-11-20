# Third-party notices

## Cosmic Peanut

The nine sound visuals of the sound preview are derived from the visualizers of Cosmic Peanut (also called Tidalviz) by Chris Buchert: they follow the pictures of its Cosmic Peanut, Pulsar, Bars, Orbit, template, Radar, Eclipse, Dark Sun and Stargate visualizers and carry over parts of their math, namely the windowing, band mapping and decibel scale of its audio analysis, the ring building, ring clock, peak tracking, taper and camera of Cosmic Peanut, the ridge envelope, scroll rate, gain and line shaping of Pulsar, the level smoothing, peak caps, flash limiter and layout shares of Bars, the pillar ring, camera path, core, reflections and fog of Orbit, the ring radius, ripple, core glow and tone mapping of Halo, the sweep, afterglow, logarithmic radius, scope layout and line shading of Radar, the level and swell smoothing, disc, ring, flare and filament formulas of Eclipse, the sun drive, diamond, surge, glow, range layout and shading of Dark Sun, and the slit-scan row motion of Stargate.

```text
MIT License

Copyright (c) 2026 Chris Buchert

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## libbink2

Bink 2 video and Bink Audio 2 play through a pinned copy of libbink2 (https://github.com/raynorpat/libbink2, commit
`ae2e41335235610ba6eff1c5124b7ee8c77165f9`) in `subprojects/libbink2`, linked statically into Luciole when playback
is built. libbink2 is licensed under the GNU Lesser General Public License, version 2.1 or (at your option) any later
version; its license text is `subprojects/libbink2/LICENSE`. Luciole's own code stays under its own license. The
copy carries local changes, listed in `subprojects/libbink2/README.md` and noted at the top of each changed file.
The sources of the linked copy are all in that folder, so a modified copy of them can be built into Luciole in its
place.

The decoder is derived from the unmerged FFmpeg Bink 2 video decoder by Konstantin Shishkov (copyright 2014) and
Paul B Mahol (copyright 2019), and from FFmpeg's `binkaudio.c` by Peter Ross (copyright 2007-2011) and Daniel
Verkamp (copyright 2009), all licensed under the GNU Lesser General Public License, version 2.1 or later.
