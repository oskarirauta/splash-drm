# Vendored libdrm headers

These are libdrm's UAPI / userspace headers, vendored so splash-drm builds with
**no system `libdrm` / `libdrm-dev` installed**. The Makefile adds `-Idrm
-Idrm/libdrm` and never adds a system include path, so the self-contained,
runtime-libdrm-free build is the one that actually gets tested.

They are unmodified copies from libdrm and keep their original **MIT / X11**
licences — see the notice at the top of each header. That is compatible with
splash-drm's own MIT licence.
