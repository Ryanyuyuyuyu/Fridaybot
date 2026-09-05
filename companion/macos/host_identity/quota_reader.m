// SPDX-License-Identifier: MIT
#import "quota_reader.h"
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

static NSError *QuotaError(NSString *message)
{
    return [NSError errorWithDomain:@"CodexHostQuota" code:1 userInfo:@{NSLocalizedDescriptionKey: message}];
}

NSString *FindCodexExecutable(void)
{
    NSMutableArray<NSString *> *candidates = [NSMutableArray array];
    for (NSString *directory in [NSProcessInfo.processInfo.environment[@"PATH"] componentsSeparatedByString:@":"]) {
        if (directory.length && [directory isAbsolutePath]) [candidates addObject:[directory stringByAppendingPathComponent:@"codex"]];
    }
    [candidates addObjectsFromArray:@[@"/Applications/Codex.app/Contents/Resources/codex",
                                     @"/Applications/ChatGPT Classic.app/Contents/Resources/codex",
                                     @"/Applications/ChatGPT.app/Contents/Resources/codex",
                                     @"/opt/homebrew/bin/codex", @"/usr/local/bin/codex"]];
    for (NSString *path in candidates) if ([NSFileManager.defaultManager isExecutableFileAtPath:path]) return path;
    return nil;
}

static BOOL IsNumber(id value)
{
    return [value isKindOfClass:NSNumber.class] && CFGetTypeID((__bridge CFTypeRef)value) != CFBooleanGetTypeID() &&
           isfinite([value doubleValue]);
}

static NSDictionary *Window(id object)
{
    if (![object isKindOfClass:NSDictionary.class]) return nil;
    id used = object[@"usedPercent"], duration = object[@"windowDurationMins"];
    if (!IsNumber(used) || !IsNumber(duration) || [duration doubleValue] <= 0 ||
        floor([duration doubleValue]) != [duration doubleValue]) return nil;
    return object;
}

static void AppendWindows(NSMutableArray *windows, id bucket)
{
    if (![bucket isKindOfClass:NSDictionary.class]) return;
    for (NSString *key in @[@"primary", @"secondary"]) {
        NSDictionary *window = Window(bucket[key]);
        if (window) [windows addObject:window];
    }
}

static NSArray *Matches(NSArray *windows, double minutes)
{
    NSPredicate *predicate = [NSPredicate predicateWithBlock:^BOOL(NSDictionary *window, NSDictionary *bindings) {
        (void)bindings;
        return fabs([window[@"windowDurationMins"] doubleValue] - minutes) <= 1;
    }];
    return [windows filteredArrayUsingPredicate:predicate];
}

static double Used(NSDictionary *window) { return fmin(100.0, fmax(0.0, [window[@"usedPercent"] doubleValue])); }

NSData *QuotaDataFromResult(NSDictionary *result, NSTimeInterval now, NSError **error)
{
    id rawBuckets = result[@"rateLimitsByLimitId"];
    NSDictionary *buckets = [rawBuckets isKindOfClass:NSDictionary.class] ? rawBuckets : @{};
    id core = buckets[@"codex"];
    if (![core isKindOfClass:NSDictionary.class]) {
        NSMutableArray *matching = [NSMutableArray array];
        for (id candidate in buckets.allValues) {
            if ([candidate isKindOfClass:NSDictionary.class] && [candidate[@"limitId"] isEqual:@"codex"]) [matching addObject:candidate];
        }
        core = matching.count == 1 ? matching.firstObject : nil;
    }
    id legacy = result[@"rateLimits"];
    if (!core && [legacy isKindOfClass:NSDictionary.class] && [legacy[@"limitId"] isEqual:@"codex"]) core = legacy;
    NSMutableArray *coreWindows = [NSMutableArray array], *nonCore = [NSMutableArray array];
    AppendWindows(coreWindows, core);
    for (NSString *key in buckets) {
        id candidate = buckets[key];
        if (![key isEqual:@"codex"] && [candidate isKindOfClass:NSDictionary.class] &&
            ![candidate[@"limitId"] isEqual:@"codex"]) AppendWindows(nonCore, candidate);
    }
    NSArray *shortMatches = Matches(coreWindows, 300), *weekMatches = Matches(coreWindows, 10080);
    NSDictionary *shortWindow = shortMatches.count == 1 ? shortMatches.firstObject : nil;
    if (shortMatches.count == 0) {
        NSArray *fallback = Matches(nonCore, 300);
        if (fallback.count == 1) shortWindow = fallback.firstObject;
    }
    NSDictionary *weekly = weekMatches.count == 1 ? weekMatches.firstObject : nil;
    NSDictionary *primary = [core isKindOfClass:NSDictionary.class] ? Window(core[@"primary"]) : nil;
    NSDictionary *selected = shortWindow ?: primary ?: coreWindows.firstObject;
    if (!selected) { if (error) *error = QuotaError(@"No unambiguous Codex rate-limit window is available."); return nil; }
    double delta = IsNumber(selected[@"resetsAt"]) ? [selected[@"resetsAt"] doubleValue] - now : 0;
    uint32_t reset = (uint32_t)fmin(UINT32_MAX, fmax(0, delta));
    NSMutableDictionary *payload = [@{@"version": @1, @"remaining_percent": @(100.0 - Used(selected)),
                                      @"reset_in_seconds": @(reset)} mutableCopy];
    if (shortWindow) payload[@"five_hour_used_percent"] = @(Used(shortWindow));
    if (weekly) payload[@"weekly_used_percent"] = @(Used(weekly));
    NSData *data = [NSJSONSerialization dataWithJSONObject:payload options:NSJSONWritingSortedKeys error:error];
    if (data.length >= 256) { if (error) *error = QuotaError(@"Quota exceeds the USB report size."); return nil; }
    return data;
}

static BOOL Send(NSFileHandle *input, NSDictionary *request, NSError **error)
{
    NSData *json = [NSJSONSerialization dataWithJSONObject:request options:0 error:error];
    if (!json) return NO;
    NSMutableData *line = [json mutableCopy];
    [line appendBytes:"\n" length:1];
    return [input writeData:line error:error];
}

static NSDictionary *Receive(NSFileHandle *output, NSMutableData *buffer, NSInteger requestID,
                             NSTimeInterval timeout, NSError **error)
{
    NSTimeInterval deadline = NSProcessInfo.processInfo.systemUptime + timeout;
    while (NSProcessInfo.processInfo.systemUptime < deadline) {
        const uint8_t *bytes = buffer.bytes;
        const uint8_t *newline = memchr(bytes, '\n', buffer.length);
        if (newline) {
            NSUInteger length = (NSUInteger)(newline - bytes);
            NSData *line = [buffer subdataWithRange:NSMakeRange(0, length)];
            [buffer replaceBytesInRange:NSMakeRange(0, length + 1) withBytes:NULL length:0];
            id object = [NSJSONSerialization JSONObjectWithData:line options:0 error:nil];
            if ([object isKindOfClass:NSDictionary.class] && IsNumber(object[@"id"]) && [object[@"id"] integerValue] == requestID) return object;
            continue; // Notifications are deliberately discarded, never logged or persisted.
        }
        struct pollfd fd = {.fd = output.fileDescriptor, .events = POLLIN};
        int remaining = (int)fmax(1, (deadline - NSProcessInfo.processInfo.systemUptime) * 1000);
        int status = poll(&fd, 1, remaining);
        if (status < 0 && errno == EINTR) continue;
        if (status <= 0) break;
        uint8_t chunk[4096];
        ssize_t count = read(fd.fd, chunk, sizeof(chunk));
        if (count <= 0) break;
        [buffer appendBytes:chunk length:(NSUInteger)count];
        if (buffer.length > 1024 * 1024) break;
    }
    if (error) *error = QuotaError(@"Codex app-server did not return a valid response before the timeout.");
    return nil;
}

NSData *ReadCodexQuota(NSString *executable, NSError **error)
{
    // A child exiting while we write must produce an NSError, not kill the helper.
    signal(SIGPIPE, SIG_IGN);
    if (!executable.length || ![NSFileManager.defaultManager isExecutableFileAtPath:executable]) {
        if (error) *error = QuotaError(@"Codex executable unavailable; use --codex-bin with its absolute path.");
        return nil;
    }
    NSTask *task = [NSTask new];
    NSPipe *input = [NSPipe pipe], *output = [NSPipe pipe];
    task.executableURL = [NSURL fileURLWithPath:executable];
    task.arguments = @[@"app-server", @"--listen", @"stdio://"];
    task.standardInput = input; task.standardOutput = output;
    task.standardError = NSFileHandle.fileHandleWithNullDevice;
    if (![task launchAndReturnError:error]) return nil;
    @try {
        NSMutableData *buffer = [NSMutableData data];
        NSDictionary *initialize = @{@"id": @0, @"method": @"initialize", @"params": @{
            @"clientInfo": @{@"name": @"codex_host_identity", @"title": @"Codex Host Identity", @"version": @"0.1.0"},
            @"capabilities": @{@"optOutNotificationMethods": @[@"item/agentMessage/delta", @"item/reasoning/textDelta"]}}};
        if (!Send(input.fileHandleForWriting, initialize, error)) return nil;
        NSDictionary *hello = Receive(output.fileHandleForReading, buffer, 0, 12, error);
        if (!hello) return nil;
        if (hello[@"error"]) { if (error) *error = QuotaError(@"Codex app-server rejected initialization."); return nil; }
        if (!Send(input.fileHandleForWriting, @{@"method": @"initialized", @"params": @{}}, error) ||
            !Send(input.fileHandleForWriting, @{@"id": @1, @"method": @"account/rateLimits/read", @"params": @{}}, error)) return nil;
        NSDictionary *response = Receive(output.fileHandleForReading, buffer, 1, 20, error);
        if (!response) return nil;
        if (response[@"error"] || ![response[@"result"] isKindOfClass:NSDictionary.class]) {
            if (error) *error = QuotaError(@"Codex rate-limit read failed; confirm this Mac is signed in to Codex.");
            return nil;
        }
        return QuotaDataFromResult(response[@"result"], NSDate.date.timeIntervalSince1970, error);
    } @finally {
        [input.fileHandleForWriting closeFile];
        if (task.running) [task terminate];
        // The child receives EOF and SIGTERM. Bound cleanup even if it fails to exit.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            if (task.running) kill(task.processIdentifier, SIGKILL);
        });
        [task waitUntilExit];
        [output.fileHandleForReading closeFile];
    }
}

BOOL TestQuotaEncoding(void)
{
    NSDictionary *hour = @{@"usedPercent": @25.5, @"windowDurationMins": @300, @"resetsAt": @1200};
    NSDictionary *week = @{@"usedPercent": @80, @"windowDurationMins": @10080, @"resetsAt": @9000};
    NSDictionary *core = @{@"limitId": @"codex", @"primary": hour, @"secondary": week};
    NSError *error = nil;
    NSData *data = QuotaDataFromResult(@{@"rateLimitsByLimitId": @{@"codex": core}}, 1000, &error);
    NSDictionary *decoded = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (!decoded || error || data.length >= 256 || ![decoded[@"remaining_percent"] isEqual:@74.5] ||
        ![decoded[@"reset_in_seconds"] isEqual:@200] || ![decoded[@"five_hour_used_percent"] isEqual:@25.5] ||
        ![decoded[@"weekly_used_percent"] isEqual:@80]) return NO;
    if (QuotaDataFromResult(@{}, 1000, &error) != nil) return NO;
    NSDictionary *invalid = @{@"limitId": @"codex", @"primary": @{@"usedPercent": @YES, @"windowDurationMins": @300}};
    if (QuotaDataFromResult(@{@"rateLimits": invalid}, 1000, &error) != nil) return NO;
    NSDictionary *ambiguous = @{@"limitId": @"codex", @"primary": hour, @"secondary": hour};
    data = QuotaDataFromResult(@{@"rateLimits": ambiguous}, 1000, nil);
    decoded = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (!decoded || decoded[@"five_hour_used_percent"] || decoded[@"weekly_used_percent"]) return NO;
    data = QuotaDataFromResult(@{@"rateLimits": core}, 2000, nil);
    decoded = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    return [decoded[@"reset_in_seconds"] isEqual:@0];
}
