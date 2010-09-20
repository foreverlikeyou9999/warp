//
//  warpAppDelegate.m
//  warp
//
//  Created by Nicholas Kostelnik on 11/09/2010.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//

#import "warpAppDelegate.h"
#import <Carbon/Carbon.h>

@implementation WarpAppDelegate

@synthesize window;

int port = 6745;

- (NSString*)recent_path {
	NSString* path = [[NSString alloc] initWithFormat:@"%@/Contents/Resources/recent.plist", [[NSBundle mainBundle] bundlePath]];
	if (![[[NSFileManager alloc ]init]fileExistsAtPath:path isDirectory:FALSE]) {
		[[[NSArray alloc] init]writeToFile:path atomically:YES];
	}
	return path;
}

- (void)server_update {
	while (server->receive());
}

- (IBAction)show_connect:(id)sender {
	[[NSApplication sharedApplication] activateIgnoringOtherApps : YES];
	[connect_window makeKeyAndOrderFront:self];
}

- (void)recent:(id)sender {
	NSMenuItem* menu_item = sender;
	
	[[recent_menu submenu]removeItem:menu_item];
	[[recent_menu submenu]addItem:menu_item];
	
	black_hole->send_to([[menu_item title] cStringUsingEncoding:NSASCIIStringEncoding], port);
}

- (IBAction)connect:(id)sender {
	[connect_window orderOut:self];
	
	if (black_hole->send_to([[address stringValue] cStringUsingEncoding:NSASCIIStringEncoding], port))
	{	
		NSMenuItem *empty_item = [[recent_menu submenu] itemWithTitle:@"Empty"];
		
		if (empty_item)
		{
			[[recent_menu submenu] removeItem:empty_item];
		}
		
		NSMenuItem *old_item = [[recent_menu submenu] itemWithTitle:[address stringValue]];
		
		if (old_item)
		{
			[[recent_menu submenu] removeItem:old_item];
		}
		
		[[recent_menu submenu] addItemWithTitle:[address stringValue] action:@selector(recent:) keyEquivalent:@""];
		[recent_list addObject:[address stringValue]];
		[recent_list writeToFile:[self recent_path] atomically:YES];
		
		if ([menu numberOfItems] > 5)
		{
			[menu removeItemAtIndex:0];
		}
	}
	else 
	{
		[connect_window makeKeyAndOrderFront:self]; 
	}
}

- (IBAction)quit:(id)sender {
	exit(0);
}

- (void)screenIsLocked {
	black_hole->disable();
}

- (void)applicationDidFinishLaunching:(NSNotification *)aNotification {	
	
	server = new Server();
	server->start_listening(port);
	
	client = new Client();
	[input_view set_client:client];
	
	black_hole = new BlackHole(client, window);
	black_hole->init();
	
	[NSThread detachNewThreadSelector:@selector(server_update) toTarget:self withObject:nil];
	
	NSStatusItem* statusItem = [[[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength] retain];
	[statusItem setImage:[NSImage imageNamed:@"menu"]];
	[statusItem setHighlightMode:YES];
	[statusItem setEnabled:YES];
	[statusItem setMenu:menu];
	
	NSDistributedNotificationCenter * center = [NSDistributedNotificationCenter defaultCenter];	
	[center addObserver:self selector:@selector(screenIsLocked) name:@"com.apple.screenIsLocked" object:nil];
	
	recent_list = [[NSMutableArray alloc] initWithContentsOfFile:[self recent_path]];
	
	if ([recent_list count] > 0)
	{
		[[recent_menu submenu] removeItemAtIndex:0];
	}
	
	for(NSString* item in recent_list) {
		[[recent_menu submenu] addItemWithTitle:item action:@selector(recent:) keyEquivalent:@""];
	}
}

@end
