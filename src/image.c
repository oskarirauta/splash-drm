/*
 * image.c - Image loading and management
 * 
 * Uses stb_image.h for PNG/JPEG decoding.
 * Single-file, public domain library.
 */

#include "splash.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

/* ========================================================================
 * Image Loading
 * ======================================================================== */

int load_image(const char *path, image_t *img) {
    if (!path || !img) return -1;
    
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) { close(fd); return -1; }
    lseek(fd, 0, SEEK_SET);
    
    uint8_t *data = malloc(sz);
    if (!data) { close(fd); return -1; }
    
    if (read(fd, data, sz) != sz) {
        free(data);
        close(fd);
        return -1;
    }
    close(fd);
    
    int comp;
    img->rgba = stbi_load_from_memory(data, (int)sz, &img->w, &img->h, &comp, 4);
    free(data);
    
    if (!img->rgba) return -1;
    return 0;
}

void free_image(image_t *img) {
    if (!img) return;
    if (img->rgba) {
        stbi_image_free(img->rgba);
        img->rgba = NULL;
    }
    img->w = img->h = 0;
}
