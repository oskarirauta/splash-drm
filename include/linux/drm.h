/*
 * Minimal DRM UAPI header for splash-drm
 * 
 * Provides ONLY the definitions needed for KMS/Modeset.
 * All legacy DRI1 types excluded to avoid conflicts.
 */

#ifndef _DRM_H_
#define _DRM_H_

#include <stdint.h>
#include <sys/types.h>

/* DRM ioctl encoding */
#define DRM_IOCTL_BASE          'd'
#define DRM_IO(nr)              _IO(DRM_IOCTL_BASE, nr)
#define DRM_IOR(nr, type)       _IOR(DRM_IOCTL_BASE, nr, type)
#define DRM_IOW(nr, type)       _IOW(DRM_IOCTL_BASE, nr, type)
#define DRM_IOWR(nr, type)      _IOWR(DRM_IOCTL_BASE, nr, type)

/* Version ioctl */
#define DRM_IOCTL_VERSION       DRM_IOWR(0x00, struct drm_version)

struct drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    size_t name_len;
    char *name;
    size_t date_len;
    char *date;
    size_t desc_len;
    char *desc;
};

/* Connection status */
#define DRM_MODE_CONNECTED      1
#define DRM_MODE_DISCONNECTED   2
#define DRM_MODE_UNKNOWNCONNECTION 3

/* Mode-setting ioctls - using our own definitions to ensure compatibility */
#define DRM_IOCTL_MODE_GETRESOURCES     DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCONNECTOR     DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_GETENCODER       DRM_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCRTC          DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC          DRM_IOWR(0xA2, struct drm_mode_crtc_set)
#define DRM_IOCTL_MODE_ADDFB            DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_RMFB             DRM_IOWR(0xAF, uint32_t)
#define DRM_IOCTL_MODE_CREATE_DUMB      DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB         DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB     DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)

/* 
 * NOTE: We intentionally do NOT define drm_context_t or other legacy types.
 * Our application defines its own drm_context_t as a struct, which would
 * conflict with the kernel's "typedef unsigned int drm_context_t".
 */

#endif /* _DRM_H_ */
