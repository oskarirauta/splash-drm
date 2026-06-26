/*
 * version.h - The single source of truth for the splash-drm version string.
 *
 * Deliberately tiny and dependency-free: splash.h includes it for the daemon
 * and control client, and the optional ubus-progress bridge includes it
 * directly so it can report the same version without dragging in splash.h
 * (which pulls in the DRM headers, the logging layer and cJSON that the bridge
 * does not need). Bump SPLASH_VERSION here and every artifact follows.
 */

#ifndef SPLASH_VERSION_H
#define SPLASH_VERSION_H

#define SPLASH_VERSION "5.0.2"

#endif /* SPLASH_VERSION_H */
