# splash-drm

Self-contained DRM/KMS bootsplash daemon for Linux initrd. Zero external dependencies.

## Features

- **Direct DRM/KMS** via kernel ioctls (no libdrm)
- **PNG image loading** via stb_image (single-file, public domain)
- **TrueType font rendering** via stb_truetype (single-file, public domain)
- **Multiple visual elements**: text, rectangles, image overlays, progress bars
- **Named pipe command interface** for real-time updates
- **Pipe relocation** for initrd -> rootfs transition
- **Static linking ready** for minimal initrd size

## Building

### Requirements

- GCC or Clang
- Linux kernel headers (for DRM ioctls)
- stb_image.h and stb_truetype.h (single-file libraries)

### Get dependencies

```bash
wget https://raw.githubusercontent.com/nothings/stb/master/stb_image.h -O include/stb_image.h
wget https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h -O include/stb_truetype.h
```

### Build

```bash
# Dynamic linking (development)
make

# Static linking (initrd)
make static

# Or manually:
gcc -O2 -static -o splash-drm src/*.c -lm -I./include
```

## Usage

```bash
./splash-drm /dev/dri/card0 /run/splash.pipe /path/to/font.ttf [options]
```

### Options

- `-bg <path> [mode]` - Initial background image (mode: cover/contain/stretch/none)
- `-text <id> <x> <y> <align> <color> <text>` - Initial text element

### Commands (via named pipe)

| Command | Description |
|---------|-------------|
| `EXIT` | Terminate daemon |
| `RELOCATE_PIPE <path>` | Move pipe to new rootfs |
| `READY?` | Check if daemon is responsive |
| `CLEAR [#RRGGBB]` | Clear screen (optional color) |
| `IMAGE <path> [mode]` | Set background image |
| `TEXT <id> <x> <y> <L/C/R> <#RRGGBB> <text>` | Add/update text |
| `REMOVE_TEXT <id>` | Remove text element |
| `RECT <id> <x> <y> <w> <h> <#RRGGBB> [blend]` | Draw rectangle |
| `REMOVE_RECT <id>` | Remove rectangle |
| `OVERLAY <id> <x> <y> [w] [h] [align] [valign] <path>` | Add image overlay |
| `REMOVE_OVERLAY <id>` | Remove overlay |
| `PROGRESS <id> <x> <y> <w> <h> <style> <prefix> <suffix> <value>` | Create progress bar |
| `UPDATE_PROGRESS <id> <value> [text]` | Update progress |
| `HIDE_PROGRESS <id>` | Hide progress bar |

### Progress Bar Styles

- `0` - Blue (modern)
- `1` - Green (success)
- `2` - Amber (warning)
- `3` - Red (error)
- `4` - Purple (accent)
- `5` - Cyan (cool)

## Initrd Integration

### Example initramfs hook

```bash
#!/bin/sh
# /usr/share/initramfs-tools/hooks/splash

PREREQ=""
prereqs() {
    echo "$PREREQ"
}

case $1 in
    prereqs)
        prereqs
        exit 0
        ;;
esac

. /usr/share/initramfs-tools/hook-functions

copy_exec /usr/bin/splash-drm
mkdir -p $DESTDIR/usr/share/fonts
cp -r /usr/share/fonts/truetype $DESTDIR/usr/share/fonts/
```

### Example initramfs script

```bash
#!/bin/sh
# /usr/share/initramfs-tools/scripts/init-premount/splash

PREREQ="udev"
prereqs() {
    echo "$PREREQ"
}

case $1 in
    prereqs)
        prereqs
        exit 0
        ;;
esac

# Start splash daemon
/usr/bin/splash-drm /dev/dri/card0 /run/splash.pipe \
    /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
    -bg /boot/splash.png &

# Wait for pipe
for i in $(seq 1 50); do
    if [ -p /run/splash.pipe ]; then
        break
    fi
    sleep 0.1
done
```

### Rootfs transition

```bash
# Before switching root
mkdir -p /newroot/run
echo "RELOCATE_PIPE /newroot/run/splash.pipe" > /run/splash.pipe

# After switching root, from new init system
echo "UPDATE_PROGRESS 0 100" > /run/splash.pipe
echo "EXIT" > /run/splash.pipe
```

## Project Structure

```
splash-drm/
├── include/
│   ├── splash.h          # Main header
│   ├── stb_image.h       # Image decoder (external)
│   └── stb_truetype.h    # Font rasterizer (external)
├── src/
│   ├── main.c            # Entry point
│   ├── drm.c             # DRM/KMS interface
│   ├── render.c          # Graphics rendering
│   ├── font.c            # Font loading/rendering
│   ├── image.c           # Image loading
│   ├── elements.c        # Element management
│   ├── pipe.c            # Named pipe I/O
│   ├── cmd.c             # Command protocol
│   └── utils.c           # Utilities
├── Makefile
└── README.md
```

## License

This project is released into the public domain (Unlicense).
The included stb libraries are also public domain.
