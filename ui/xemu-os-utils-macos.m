/*
 * OS-specific Helpers
 *
 * Copyright (C) 2020 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#include <sys/resource.h>
#include "xemu-os-utils.h"

/* Private <sys/resource.h> values (xnu bsd/sys/resource.h). */
#ifndef PRIO_DARWIN_ROLE
#define PRIO_DARWIN_ROLE 6
#endif
#ifndef PRIO_DARWIN_ROLE_UI
#define PRIO_DARWIN_ROLE_UI 0x2
#endif

/*
 * After a crash of any app named xemu, AppKit's next launch asks whether
 * to reopen its windows: a modal alert run on the main thread while it
 * handles the launch event, invisible when headless. That thread serves
 * screendump and quit, so both stall until someone answers. SDL registers
 * ApplePersistenceIgnoreState, but only after that check. xemu has no
 * windows to restore, so register it before main() runs.
 */
__attribute__((constructor))
static void xemu_os_ignore_persistent_state(void)
{
	@autoreleasepool {
		[[NSUserDefaults standardUserDefaults]
			registerDefaults:@{ @"ApplePersistenceIgnoreState" : @YES }];
	}
}

const char *xemu_get_os_info(void)
{
	return [[[NSProcessInfo processInfo] operatingSystemVersionString] UTF8String];
}

void xemu_os_set_app_role(void)
{
	/*
	 * A process started from an app (Terminal, an agent's app) inherits
	 * the app type but no role, and the kernel squashes the
	 * USER_INTERACTIVE and USER_INITIATED threads of a roleless app to
	 * the legacy priority (31, which decays under load). AppKit's check-in
	 * gets offscreen xemu the role UI from RunningBoard (an unfocused
	 * window gets UI non-focal); null mode starts no Cocoa app, so take
	 * offscreen's role here, or its vCPU waits for a core most of the time
	 * on a loaded host. A daemon-type process (a launchd job) gets ENOTSUP
	 * and changes nothing: its launchd ProcessType caps its QoS instead.
	 */
	setpriority(PRIO_DARWIN_ROLE, 0, PRIO_DARWIN_ROLE_UI);
}

void xemu_os_set_background_app(void)
{
	[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

	/*
	 * A windowless app is an App Nap candidate: its timers get coalesced,
	 * the vblank thread oversleeps and the guest crawls. Opt out for the
	 * life of the process.
	 */
	static id activity;
	activity = [[[NSProcessInfo processInfo]
		beginActivityWithOptions:NSActivityUserInitiated |
		                         NSActivityLatencyCritical
		                  reason:@"xemu headless emulation"] retain];
}
