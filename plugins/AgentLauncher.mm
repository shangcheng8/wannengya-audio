#include "AgentLauncher.h"

#include "Config.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

namespace wng {

bool agentRunning()
{
    NSArray<NSRunningApplication*>* apps =
        [NSRunningApplication runningApplicationsWithBundleIdentifier:@(kAgentBundleID)];

    return apps.count > 0;
}

bool launchAgentIfNeeded()
{
    if (agentRunning())
        return true;

    NSFileManager* manager = [NSFileManager defaultManager];

    NSArray<NSString*>* candidates = @[
        @"/Applications/wannengya Agent.app",
        [@"~/Applications/wannengya Agent.app" stringByExpandingTildeInPath],
        @"/Library/Application Support/wannengya/wannengya Agent.app"
    ];

    for (NSString* path in candidates)
    {
        if (! [manager fileExistsAtPath:path])
            continue;

        NSWorkspaceOpenConfiguration* configuration = [NSWorkspaceOpenConfiguration configuration];
        configuration.activates = NO;
        configuration.addsToRecentItems = NO;
        configuration.createsNewApplicationInstance = NO;

        [[NSWorkspace sharedWorkspace] openApplicationAtURL:[NSURL fileURLWithPath:path]
                                             configuration:configuration
                                         completionHandler:nil];
        return true;
    }

    return false;
}

} // namespace wng
