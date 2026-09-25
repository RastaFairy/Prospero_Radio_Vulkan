# Upstream notice

ProsperoRadio is an open-source GPL-3.0-or-later PS5 homebrew project by `blackbearreloaded`.

This package intentionally materializes the upstream project at build time instead of redistributing a second copy of its large checked-in asset tree.

Upstream repository:
https://github.com/blackbearreloaded/ProsperoRadio

Pinned commit:
`33898dd35375c1ae8370da137cfb6941d91c7684`

This fork adds an optional Vulkan rendering path, a physical-radio UI overlay, the 4K KTX2 backplate, and the scripts that build the pinned upstream source. It does not copy or replace the upstream source tree; the build materializes that exact commit into a separate cache before applying the overlay.
