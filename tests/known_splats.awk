# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
#
# known_splats.awk -- set aside kernel splats caused by known upstream bugs
#
# Prints a kernel log back with every "cut here" ... "end trace" block and
# every KASAN report ("====" ... "====") that matches a known upstream
# (non-ftdi) issue replaced by one line:
#
#	KNOWN UPSTREAM ISSUE: <tag>
#
# Any other splat goes through untouched. Used by init.sh (in the guest)
# and check_log.sh (host).
#
# gpiolib-cdev-irq-double-free (since v6.13, still in v7.4):
#	gpiochip_remove() frees the IRQs that consumers still hold (commit
#	ec8b6f55b981); closing a userspace edge request while or after that
#	calls free_irq() a second time.
#	Matched: "Trying to free already-free IRQ" under edge_detector_stop().
#
# uml-kasan-stale-module-shadow (UML + KASAN):
#	vmalloc space where an unloaded module lived keeps its global
#	redzone poison (kasan_free_module_shadow() is a no-op on UML) and
#	the module loader trips on it when modules are reloaded.
#	Matched: a global-out-of-bounds in a vmalloc region, under
#	load_module(), with no ftdi function in the trace.

function known(b)
{
	if (b ~ /Trying to free already-free IRQ/ && b ~ /edge_detector_stop/)
		return "gpiolib-cdev-irq-double-free"
	if (b ~ /BUG: KASAN: global-out-of-bounds/ && b ~ /load_module/ &&
	    b ~ /vmalloc region/ && b !~ /\[ftdi_/)
		return "uml-kasan-stale-module-shadow"
	return ""
}

function close_blk()
{
	inblk = ""
	tag = known(blk)
	if (tag != "")
		print "KNOWN UPSTREAM ISSUE: " tag
	else
		printf "%s", blk
}

# A KASAN report starts with a line of '=' just before "BUG: KASAN:"
pending != "" {
	if ($0 ~ /BUG: KASAN:/ && !inblk) {
		blk = pending "\n" $0 "\n"
		inblk = "kasan"
		pending = ""
		next
	}
	if (inblk)
		blk = blk pending "\n"
	else
		print pending
	pending = ""
}

/={60}/ && inblk != "kasan" {
	pending = $0
	next
}

/cut here \]/ && !inblk {
	blk = $0 "\n"
	inblk = "warn"
	next
}

inblk {
	blk = blk $0 "\n"
	if ((inblk == "warn" && $0 ~ /end trace/) ||
	    (inblk == "kasan" && $0 ~ /={60}/))
		close_blk()
	next
}

{ print }

END {
	if (pending != "") {
		if (inblk)
			blk = blk pending "\n"
		else
			print pending
	}
	if (inblk)
		printf "%s", blk
}
