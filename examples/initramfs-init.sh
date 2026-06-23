#!/bin/busybox sh
#
# examples/initramfs-init.sh — example initramfs /init that boots behind a
# splash-drm bootsplash, including from slow USB / external media.
#
# As PID 1 in an initramfs it:
#   1. mounts /proc, /sys and a devtmpfs /dev,
#   2. starts the splash-drm bootsplash on /dev/dri/card0,
#   3. waits for the root filesystem named by root= to appear, then mounts it,
#   4. suspends the splash and switch_roots into the real system.
#
# WHY THE WAIT (the point of this example): with a monolithic kernel (storage
# and USB drivers built in) plus devtmpfs, block-device nodes are created
# automatically as the kernel enumerates hardware. Local SATA/NVMe disks are
# ready by the time init runs, but USB / external media take a couple of
# seconds, so a single blkid at boot misses them and the mount fails. We instead
# poll by the root= identifier (UUID / PARTUUID / LABEL) until it shows up or a
# timeout elapses — one loop covers local (resolves on the first pass) and USB
# (resolves a few passes later) with no module loading and no fixed /dev names.
#
# Assumes util-linux blkid (the standard on this initramfs); a busybox-only
# blkid may need `findfs` instead. Copy to your initramfs as /init and adjust
# the ROOT_WAIT_* values and the splash --config scene document to taste.

PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PATH

# Root-device wait budget: up to ROOT_WAIT_TRIES passes, ROOT_WAIT_SECS apart.
ROOT_WAIT_TRIES=30
ROOT_WAIT_SECS=1

# Boot failed. Small panels render the kernel console microscopically, so show
# the reason large on the splash first and hold it a few seconds; then release
# the display (restoring the console) and drop to a rescue shell for whoever can
# read it. (splash-drm doesn't hide the text cursor the way old framebuffer
# splashes did, so there is nothing to restore on the way out.) The dynamic
# reason is passed with -D, so splash-ctl substitutes ${MSG} and JSON-escapes
# it safely — the message may contain any characters, quotes included.
fail() {
	splash-ctl '{"system":{"action":"clear","color":"#2a0000"}}' 2>/dev/null
	splash-ctl '{"text":{"id":0,"y":"40%","size":56,"font":1,"color":"#ff6060","text":"Boot failed"}}' 2>/dev/null
	splash-ctl '{"text":{"id":1,"y":"56%","size":28,"color":"#ffd6d6","wrap":true,"wrap_width":700,"text":"${MSG}"}}' -D MSG="$1" 2>/dev/null
	sleep 4
	splash-ctl '{"system":"exit"}' 2>/dev/null
	printf '\n%s\nDropping to a rescue shell.\n' "$1" > /dev/tty1
	exec </dev/tty1 >/dev/tty1 2>/dev/tty1
	exec /bin/busybox sh
}

# Pseudo-filesystems.
mount -t proc     none /proc
mount -t sysfs    none /sys
mount -t devtmpfs none /dev

# Start the bootsplash. --fork detaches cleanly; --timeout is a safety net so a
# stuck boot can never leave the daemon up forever. Fonts/images resolve from
# the default /usr/share/splash search paths.
splash-drm /dev/dri/card0 --fork --timeout 900 \
	--config '{"version":1,"fonts":[{"slot":0,"path":"regular.ttf","size":24},{"slot":1,"path":"bold.ttf","size":24}],"background":"#000","elements":[{"image":{"path":"splash.png","mode":3}},{"text":{"id":0,"font":0,"y":"70%","size":76,"shadow":1,"text":"Booting!"}}]}'

# The root= identifier from the kernel command line (UUID=, PARTUUID=, LABEL= or
# a /dev path). Walk the tokens so a stray "root" elsewhere can't confuse it.
ROOTSPEC=
for tok in $(cat /proc/cmdline); do
	case "$tok" in
		root=*) ROOTSPEC=${tok#root=} ;;
	esac
done
[ -n "$ROOTSPEC" ] || fail "no root= on the kernel command line"

# Resolve the root= identifier to a block device, or print nothing if it is not
# present yet. Handles an explicit /dev node, a TAG=value (UUID/PARTUUID/LABEL),
# or a bare value taken as a UUID.
resolve_root() {
	case "$1" in
		/dev/*)  [ -b "$1" ] && echo "$1" ;;
		*=*)     blkid -l -t "$1" -o device 2>/dev/null ;;
		*)       blkid -U "$1" 2>/dev/null ;;
	esac
}

# Poll until the device appears (covers slow USB/external media) or give up.
ROOTDEV=
n=0
while [ "$n" -lt "$ROOT_WAIT_TRIES" ]; do
	ROOTDEV=$(resolve_root "$ROOTSPEC")
	[ -n "$ROOTDEV" ] && [ -b "$ROOTDEV" ] && break
	# On the first miss, note the wait on screen (merges into the existing
	# text element, keeping its position/size); skipped for instant local disks.
	[ "$n" = 0 ] && splash-ctl '{"text":{"id":0,"text":"Waiting for storage…"}}' 2>/dev/null
	n=$((n + 1))
	sleep "$ROOT_WAIT_SECS"
done
[ -n "$ROOTDEV" ] && [ -b "$ROOTDEV" ] || \
	fail "Root device not found after $((ROOT_WAIT_TRIES * ROOT_WAIT_SECS))s — looked for $ROOTSPEC"

# Mount the root and carry /dev across (the new init remounts /proc and /sys).
mount "$ROOTDEV" /mnt || fail "failed to mount $ROOTDEV on /mnt"
umount /proc /sys
mount --move /dev /mnt/dev

# Freeze the splash for a flicker-free handoff, then switch_root. No delay is
# needed: the daemon presents its first frame before it begins servicing the
# control socket, and splash-ctl is synchronous, so once this returns the splash
# is on screen and frozen — it keeps DRM master across the switch. (If you give
# the splash a fade-in or animation, add a short sleep here so it settles first.)
splash-ctl '{"system":"suspend"}'

exec switch_root /mnt /sbin/init
