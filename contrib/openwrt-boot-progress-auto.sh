#!/bin/sh
#
# openwrt-boot-progress-auto.sh — AUTOMATIC splash-drm boot progress for OpenWrt
#
# This is the zero-touch companion: source it once from /etc/rc.common and
# every /etc/rc.d/S* boot script reports its own progress — no per-service
# edits. (For the manual, call-it-yourself helpers instead, see the sibling
# script openwrt-boot-progress-simple.sh.)
#
# HOW IT WORKS
# ------------
# On OpenWrt, every init script's shebang is `#!/bin/sh /etc/rc.common`, so the
# kernel runs each one as `/bin/sh /etc/rc.common <initscript> <action> ...`.
# /etc/rc.common is thus the single chokepoint all init traffic flows through;
# near its top it sets:
#
#     initscript=$1      # e.g. /etc/rc.d/S20network  (or /etc/init.d/network)
#     action=${2:-help}  # e.g. boot
#     shift 2
#
# Because rc.common is re-sourced for *each* script procd launches, sourcing
# this file right after that `shift 2` runs it exactly once per boot script,
# just before the script's action executes — the perfect spot to log
# "starting <svc>" and advance a deterministic N-of-total bar.
#
# The denominator is simply the boot-script count, `ls /etc/rc.d/S* | wc -l`.
# /tmp is a fresh tmpfs every boot, so the counters reset on their own.
#
# SELF-LIMITING (safe to leave installed forever)
# -----------------------------------------------
# Each update is a splash-ctl call that fails silently when the daemon is not
# reachable. splash-drm only runs during boot, so afterwards — e.g. a manual
# `/etc/init.d/network restart` — this code does nothing. It is likewise a
# near-no-op when splash-ctl is not installed at all.
#
# FINISHING
# ---------
# When the bar reaches 100% (the last counted S## script), an optional finish
# step runs in the background: it prints a "ready" line, fades the bar out and
# asks the daemon to exit. A one-shot flag (SPLASH_DONE_FLAG) guards against the
# bar being redrawn by the trailing non-S## init scripts that run *after* the
# counted ones. The flag and counters are cleared only once the daemon has
# exited, so a late script that slips past the guard hits a dead daemon and
# can't redraw — and a fresh splash can be started later to tweak a theme
# without rebooting. Set SPLASH_FINISH=0 to keep the splash up and manage its
# lifetime yourself.
#
# INTEGRATION (one line)
# ----------------------
# Add this to /etc/rc.common, immediately after its `shift 2`:
#
#     [ -f /usr/share/splash/openwrt-boot-progress-auto.sh ] && \
#         . /usr/share/splash/openwrt-boot-progress-auto.sh
#
# Your splash scene document (--config) must create the targets:
#   - a "progress" (or "arc") element with id=0   → the progress bar
#   - a "console"  element with id=0              → the "starting <svc>" log
# Don't want the log? Set SPLASH_CONSOLE_ID="" before sourcing. All ids, the
# bar type, the colour and the matched actions are overridable below.

# --- configuration (export/override from the environment before sourcing) ---
SPLASH_CTL=${SPLASH_CTL:-splash-ctl}
SPLASH_BAR_CMD=${SPLASH_BAR_CMD:-progress}        # "progress" or "arc"
SPLASH_BAR_ID=${SPLASH_BAR_ID:-0}
SPLASH_CONSOLE_ID=${SPLASH_CONSOLE_ID:-0}         # "" disables the log line
SPLASH_LOG_COLOR=${SPLASH_LOG_COLOR:-#9fd2ff}
SPLASH_BOOT_ACTIONS=${SPLASH_BOOT_ACTIONS:-boot start}
SPLASH_TOTAL_FILE=${SPLASH_TOTAL_FILE:-/tmp/.splash_boot_total}
SPLASH_CUR_FILE=${SPLASH_CUR_FILE:-/tmp/.splash_boot_cur}
SPLASH_DONE_FLAG=${SPLASH_DONE_FLAG:-/tmp/.splash_boot_done}
# --- finish step (runs once when the bar fills; set SPLASH_FINISH=0 to skip) --
SPLASH_FINISH=${SPLASH_FINISH:-1}                 # 0/"" leaves the splash up
SPLASH_READY_TEXT=${SPLASH_READY_TEXT:-System ready!}
SPLASH_READY_COLOR=${SPLASH_READY_COLOR:-#9fe0a0}
SPLASH_DONE_DELAY=${SPLASH_DONE_DELAY:-2}          # pause before the ready line, s
SPLASH_EXIT_DELAY=${SPLASH_EXIT_DELAY:-4}          # splash-ctl --exit timeout, s
# ---------------------------------------------------------------------------

# Fire exactly once, when the bar first fills. The flag is created
# synchronously so every boot script that runs afterwards — including the
# trailing non-S## scripts that aren't part of the denominator — is gated out
# by _splash_boot_report below.
#
# Ordering inside the background subshell matters: we request the daemon's exit
# and only THEN sleep past its timeout before removing the flag and counters.
# Because the sleep starts after --exit returns and runs one second longer than
# the daemon's own timeout, the rm lands after the daemon has gone, so even a
# trailing script that slips past the gate hits a dead daemon and can't redraw.
_splash_boot_done() {
    case "$SPLASH_FINISH" in 0|""|no|NO) return 0 ;; esac
    [ -f "$SPLASH_DONE_FLAG" ] && return 0       # only once
    : > "$SPLASH_DONE_FLAG"
    (   # background, so the delays never block the rest of init
        sleep "$SPLASH_DONE_DELAY"
        [ -n "$SPLASH_CONSOLE_ID" ] && \
            "$SPLASH_CTL" "{\"console\":{\"id\":$SPLASH_CONSOLE_ID,\"write\":\"$SPLASH_READY_TEXT\",\"color\":\"$SPLASH_READY_COLOR\"}}"
        "$SPLASH_CTL" "{\"$SPLASH_BAR_CMD\":{\"id\":$SPLASH_BAR_ID,\"animate\":{\"property\":\"opacity\",\"to\":0,\"duration\":400,\"remove_on_end\":true}}}"
        "$SPLASH_CTL" --exit --timeout "$SPLASH_EXIT_DELAY"
        sleep $((SPLASH_EXIT_DELAY + 1))         # wait until the daemon is surely gone
        rm -f "$SPLASH_TOTAL_FILE" "$SPLASH_CUR_FILE" "$SPLASH_DONE_FLAG"
    ) >/dev/null 2>&1 &
}

_splash_boot_report() {
    # Once the bar has filled, stay quiet for the rest of the boot so trailing
    # scripts don't redraw the bar or overwrite the ready line.
    [ -f "$SPLASH_DONE_FLAG" ] && return 0

    # Only act on boot-sequence actions; everything else (status, restart, …)
    # returns immediately. The action is rc.common's own $action variable.
    case " $SPLASH_BOOT_ACTIONS " in
        *" $action "*) ;;
        *) return 0 ;;
    esac
    command -v "$SPLASH_CTL" >/dev/null 2>&1 || return 0

    # Service name: basename of the init script with any rc.d S##/K## ordering
    # prefix stripped (procd may invoke us via the /etc/rc.d/S20network symlink).
    local name
    name=$(basename "$initscript" 2>/dev/null)
    name=$(printf '%s' "$name" | sed -e 's/^[SK][0-9][0-9]*//')
    [ -n "$name" ] || return 0

    # Denominator: count S## scripts once per boot, then cache it.
    local total
    if [ -s "$SPLASH_TOTAL_FILE" ]; then
        total=$(cat "$SPLASH_TOTAL_FILE")
    else
        total=$(ls /etc/rc.d/S* 2>/dev/null | wc -l)
        [ "$total" -gt 0 ] 2>/dev/null || total=1
        printf '%s\n' "$total" > "$SPLASH_TOTAL_FILE"
    fi

    # Monotonic per-boot counter. The S## scripts run sequentially through
    # rc.common, so a plain read-increment-write is good enough here.
    local cur
    cur=$(cat "$SPLASH_CUR_FILE" 2>/dev/null || echo 0)
    cur=$((cur + 1))
    printf '%s\n' "$cur" > "$SPLASH_CUR_FILE"

    local frac
    frac=$(awk "BEGIN{v=$cur/$total; if(v>1)v=1; printf \"%.4f\", v}")

    "$SPLASH_CTL" "{\"$SPLASH_BAR_CMD\":{\"id\":$SPLASH_BAR_ID,\"value\":$frac}}" \
        >/dev/null 2>&1
    [ -n "$SPLASH_CONSOLE_ID" ] && \
        "$SPLASH_CTL" "{\"console\":{\"id\":$SPLASH_CONSOLE_ID,\"write\":\"starting $name\",\"color\":\"$SPLASH_LOG_COLOR\"}}" \
            >/dev/null 2>&1

    # Last counted script → run the one-shot finish step.
    [ "$cur" -ge "$total" ] && _splash_boot_done

    return 0
}

# Never abort the init script that sourced us, whatever happens.
_splash_boot_report 2>/dev/null || true
