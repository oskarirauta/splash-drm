#!/bin/sh
#
# openwrt-boot-progress-simple.sh — MANUAL splash-drm boot progress for OpenWrt
#
# This is the explicit, call-it-yourself companion: source it and then invoke
# splash_progress_done / splash_status_text / splash_boot_done from your init
# scripts. You get full control over *when* progress advances and *what* text
# is shown. If you'd rather have every boot script report automatically with no
# per-service edits, use the sibling script instead:
#
#     openwrt-boot-progress-auto.sh
#
# HOW TO INTEGRATE
# ----------------
# Source this file near the top of /etc/rc.common (before the action functions)
# so the helper functions below are available to every init script:
#
#   [ -f /usr/share/splash/openwrt-boot-progress-simple.sh ] && \
#       . /usr/share/splash/openwrt-boot-progress-simple.sh
#
# Then call splash_progress_done once per service near the end of the start()
# or boot() action.
#
# REQUIREMENTS
# ------------
# - splash-drm daemon running (started from /etc/init.d/splash or preinit)
# - splash-ctl in PATH (usually /usr/bin/splash-ctl)
# - An arc element with id=0 already created by the startup commands
#
# SAFE TO INCLUDE UNCONDITIONALLY
# --------------------------------
# All functions check whether splash-ctl exists and whether the daemon is
# reachable before doing anything, so including this file is harmless on
# systems where splash-drm is not installed.

SPLASH_PROGRESS_COUNT_FILE=/tmp/.splash_svc_count
SPLASH_PROGRESS_ARC_ID=0

# Count the total number of boot-time (S##) services once per boot.
_splash_count_services() {
    ls /etc/rc.d/S* 2>/dev/null | wc -l
}

# Clamp a value to [0, 1] and format with 4 decimal places.
_splash_fmt() {
    awk "BEGIN { v=$1; if(v<0)v=0; if(v>1)v=1; printf \"%.4f\", v }"
}

# Call after a service has started to advance the progress bar.
# Usage: splash_progress_done [service_name]
splash_progress_done() {
    command -v splash-ctl >/dev/null 2>&1 || return 0

    local total
    total=$(_splash_count_services)
    [ "$total" -le 0 ] && return 0

    # Read and increment the per-boot counter atomically enough for a
    # single-threaded init; OpenWrt parallelises via procd but the S##
    # scripts themselves run sequentially through rc.common.
    local done
    done=$(cat "$SPLASH_PROGRESS_COUNT_FILE" 2>/dev/null || echo 0)
    done=$((done + 1))
    printf '%s\n' "$done" > "$SPLASH_PROGRESS_COUNT_FILE"

    local new_val
    new_val=$(_splash_fmt "$(awk "BEGIN{print $done/$total}")")

    # Query the daemon for the current value so we never go backwards.
    # This guards against parallel init paths (procd-launched services)
    # that might race and report an earlier percentage.
    local current_val
    current_val=$(splash-ctl "{\"cmd\":\"query\",\"type\":\"arc\",\"id\":$SPLASH_PROGRESS_ARC_ID}" \
                  2>/dev/null | grep -o '"value":[0-9.]*' | cut -d: -f2)

    if [ -n "$current_val" ]; then
        local should_update
        should_update=$(awk "BEGIN{print ($new_val > $current_val) ? 1 : 0}")
        [ "$should_update" = "1" ] || return 0
    fi

    splash-ctl "{\"cmd\":\"update_arc\",\"id\":$SPLASH_PROGRESS_ARC_ID,\"value\":$new_val}" \
        >/dev/null 2>&1 || true
}

# Optional: update the status text element (id=1) with the current service name.
# Call as: splash_status_text "Loading kernel modules"
splash_status_text() {
    command -v splash-ctl >/dev/null 2>&1 || return 0
    local msg
    msg=$(printf '%s' "$1" | sed "s/\"/'/g")   # escape quotes for JSON
    splash-ctl "{\"cmd\":\"text\",\"id\":1,\"text\":\"$msg\"}" >/dev/null 2>&1 || true
}

# Call when boot is complete (from the last init script or a dedicated
# splash-stop service).
splash_boot_done() {
    command -v splash-ctl >/dev/null 2>&1 || return 0
    splash-ctl "[
        {\"cmd\":\"update_arc\",\"id\":$SPLASH_PROGRESS_ARC_ID,\"value\":1.0},
        {\"cmd\":\"animate\",\"type\":\"arc\",\"id\":$SPLASH_PROGRESS_ARC_ID,
         \"to\":0,\"duration\":600,\"easing\":\"ease_out\",\"remove_on_end\":true},
        {\"cmd\":\"animate\",\"type\":\"text\",\"id\":1,
         \"to\":0,\"duration\":400,\"easing\":\"ease_out\",\"remove_on_end\":true}
    ]" >/dev/null 2>&1 || true
    rm -f "$SPLASH_PROGRESS_COUNT_FILE"
}

# --------------------------------------------------------------------------
# WANT THIS AUTOMATICALLY (no per-service calls)?
# --------------------------------------------------------------------------
# The sibling script openwrt-boot-progress-auto.sh does exactly that: source it
# once, immediately after rc.common's `shift 2`, and every /etc/rc.d/S* boot
# script reports its own progress and a "starting <svc>" log line — with no
# splash_progress_done() calls anywhere. Use this simple script instead when
# you want to choose the moments and messages yourself.
