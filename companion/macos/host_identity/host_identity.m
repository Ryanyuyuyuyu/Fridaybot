// SPDX-License-Identifier: MIT
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import "quota_reader.h"
#import "power_policy.h"
#import <CoreBluetooth/CoreBluetooth.h>
#import <IOKit/hid/IOHIDManager.h>
#import <SystemConfiguration/SystemConfiguration.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static const NSInteger kVendorID = 0x303A;
static const NSInteger kProductID = 0x8360;
static const uint8_t kIdentityReportID = 7;
enum { kIdentityBodySize = 128, kNameByteLimit = 31 };

static CBUUID *ServiceUUID(void) { return [CBUUID UUIDWithString:@"7F0D4E66-2AC2-4A71-BFBE-4EF61A0E5C01"]; }
static CBUUID *QuotaUUID(void) { return [CBUUID UUIDWithString:@"7F0D4E66-2AC2-4A71-BFBE-4EF61A0E5C02"]; }
static CBUUID *IdentityUUID(void) { return [CBUUID UUIDWithString:@"7F0D4E66-2AC2-4A71-BFBE-4EF61A0E5C03"]; }
static CBUUID *IdentityServiceUUID(void) { return [CBUUID UUIDWithString:@"7F0D4E66-2AC2-4A71-BFBE-4EF61A0E5C04"]; }

static NSError *Failure(NSString *message)
{
    return [NSError errorWithDomain:@"CodexHostIdentity" code:1
                          userInfo:@{NSLocalizedDescriptionKey: message}];
}

static NSString *ShortName(NSString *name, NSUInteger maxBytes)
{
    NSMutableString *result = [NSMutableString string];
    [name enumerateSubstringsInRange:NSMakeRange(0, name.length)
                            options:NSStringEnumerationByComposedCharacterSequences
                         usingBlock:^(NSString *part, NSRange range, NSRange enclosing, BOOL *stop) {
        (void)range; (void)enclosing;
        if ([part rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location != NSNotFound) {
            part = @"-";
        }
        if ([result lengthOfBytesUsingEncoding:NSUTF8StringEncoding] +
            [part lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > maxBytes) {
            *stop = YES;
        } else {
            [result appendString:part];
        }
    }];
    return result.length ? result : @"Mac";
}

static NSData *IdentityData(NSString *hostID, NSString *name, NSError **error)
{
    NSUUID *uuid = [[NSUUID alloc] initWithUUIDString:hostID];
    if (uuid == nil || hostID.length != 36) {
        if (error) *error = Failure(@"Host ID must be a canonical UUID.");
        return nil;
    }
    // JSON escaping can make a short UTF-8 name longer on the wire.
    for (NSUInteger limit = kNameByteLimit; limit > 0; --limit) {
        NSDictionary *identity = @{@"version": @1, @"hostId": uuid.UUIDString.lowercaseString,
                                    @"name": ShortName(name, limit)};
        NSData *data = [NSJSONSerialization dataWithJSONObject:identity options:NSJSONWritingSortedKeys error:error];
        if (!data) return nil;
        if (data.length < kIdentityBodySize) return data; // reserve one NUL byte for USB
    }
    if (error) *error = Failure(@"Identity cannot fit the USB feature report.");
    return nil;
}

static NSString *LocalMacName(void)
{
    NSString *name = CFBridgingRelease(SCDynamicStoreCopyLocalHostName(NULL));
    return name.length ? name : @"Mac";
}

static NSData *RecentQuota(NSData *data, NSTimeInterval age)
{
    if (!data || !isfinite(age) || age < 0 || age >= CHQuotaMinimumIntervalSeconds) return nil;
    id decoded = [NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingMutableContainers error:nil];
    if (![decoded isKindOfClass:NSMutableDictionary.class] || ![decoded[@"reset_in_seconds"] isKindOfClass:NSNumber.class]) return nil;
    double remaining = fmax(0, [decoded[@"reset_in_seconds"] doubleValue] - age);
    decoded[@"reset_in_seconds"] = @((uint32_t)remaining);
    return [NSJSONSerialization dataWithJSONObject:decoded options:NSJSONWritingSortedKeys error:nil];
}

static BOOL PreparePrivateDirectory(NSURL *directory, NSError **error)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm createDirectoryAtURL:directory withIntermediateDirectories:YES
                       attributes:@{NSFilePosixPermissions: @0700} error:error]) return NO;
    struct stat directoryAttributes;
    if (lstat(directory.fileSystemRepresentation, &directoryAttributes) != 0 ||
        !S_ISDIR(directoryAttributes.st_mode) || directoryAttributes.st_uid != getuid() ||
        chmod(directory.fileSystemRepresentation, 0700) != 0) {
        if (error) *error = Failure(@"The identity directory must be a private directory owned by the current user.");
        return NO;
    }
    return YES;
}

static NSString *LoadIdentity(NSURL *directory, NSError **error)
{
    if (!PreparePrivateDirectory(directory, error)) return nil;
    NSURL *lockURL = [directory URLByAppendingPathComponent:@"identity.lock"];
    int lockFD = open(lockURL.fileSystemRepresentation, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (lockFD < 0) {
        if (error) *error = Failure(@"Cannot open the identity lock file.");
        return nil;
    }
    if (flock(lockFD, LOCK_EX) != 0) {
        close(lockFD);
        if (error) *error = Failure(@"Cannot lock the host identity.");
        return nil;
    }
    NSURL *idURL = [directory URLByAppendingPathComponent:@"host-id"];
    NSString *hostID = nil;
    struct stat attributes;
    if (lstat(idURL.fileSystemRepresentation, &attributes) == 0) {
        if (!S_ISREG(attributes.st_mode) || attributes.st_uid != getuid()) {
            if (error) *error = Failure(@"The host-id path must be a regular file owned by the current user.");
        } else {
            hostID = [NSString stringWithContentsOfURL:idURL encoding:NSUTF8StringEncoding error:error];
            hostID = [hostID stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
            NSUUID *uuid = [[NSUUID alloc] initWithUUIDString:hostID];
            if (uuid == nil || hostID.length != 36) {
                hostID = nil;
                if (error) *error = Failure(@"Stored host-id is invalid; restore it instead of silently changing device identity.");
            } else if (chmod(idURL.fileSystemRepresentation, 0600) != 0) {
                hostID = nil;
                if (error) *error = Failure(@"Cannot protect the host-id file.");
            } else {
                hostID = uuid.UUIDString.lowercaseString;
            }
        }
    } else if (errno == ENOENT) {
        hostID = NSUUID.UUID.UUIDString.lowercaseString;
        NSData *bytes = [[hostID stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding];
        if (![bytes writeToURL:idURL options:NSDataWritingAtomic error:error]) hostID = nil;
        if (hostID && chmod(idURL.fileSystemRepresentation, 0600) != 0) {
            hostID = nil;
            if (error) *error = Failure(@"Cannot protect the new host-id file.");
        }
    } else {
        if (error) *error = Failure(@"Cannot inspect the stored host-id file.");
    }
    flock(lockFD, LOCK_UN);
    close(lockFD);
    return hostID;
}

static BOOL ValidDisplayName(NSString *name, NSError **error)
{
    if (!name.length || ![name isEqual:[name stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]] ||
        [name rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location != NSNotFound ||
        [name lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > kNameByteLimit) {
        if (error) *error = Failure(@"Display name must be 1–31 UTF-8 bytes, with no control characters or surrounding whitespace.");
        return NO;
    }
    // Explicit aliases must arrive unchanged, including when JSON escaping adds bytes.
    NSData *data = IdentityData(@"5c5b1aad-3f81-4d87-9033-7e08be65e4a8", name, error);
    NSDictionary *decoded = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:error] : nil;
    if (![decoded[@"name"] isEqual:name]) {
        if (error) *error = Failure(@"Display name cannot fit the identity report unchanged; use a shorter name.");
        return NO;
    }
    return YES;
}

typedef NS_ENUM(NSUInteger, DisplayNameOperation) { DisplayNameRead, DisplayNameWrite, DisplayNameClear };

// A missing alias is nil without an error. Explicit set/clear never touches host-id.
static NSString *DisplayNameSetting(NSURL *directory, DisplayNameOperation operation, NSString *name, NSError **error)
{
    if (operation == DisplayNameWrite && !ValidDisplayName(name, error)) return nil;
    if (!PreparePrivateDirectory(directory, error)) return nil;
    int directoryFD = open(directory.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int lockFD = directoryFD < 0 ? -1 : openat(directoryFD, "display-name.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat attributes;
    BOOL locked = lockFD >= 0 && fstat(lockFD, &attributes) == 0 && S_ISREG(attributes.st_mode) &&
                  attributes.st_uid == getuid() && attributes.st_nlink == 1 && fchmod(lockFD, 0600) == 0 &&
                  flock(lockFD, LOCK_EX) == 0;
    if (!locked) {
        if (lockFD >= 0) close(lockFD);
        if (directoryFD >= 0) close(directoryFD);
        if (error) *error = Failure(@"Cannot safely lock the display name.");
        return nil;
    }
    NSString *result = nil;
    do {
        int existing = fstatat(directoryFD, "display-name", &attributes, AT_SYMLINK_NOFOLLOW);
        if ((existing == 0 && (!S_ISREG(attributes.st_mode) || attributes.st_uid != getuid() || attributes.st_nlink != 1)) ||
            (existing != 0 && errno != ENOENT)) {
            if (error) *error = Failure(@"The display-name path must be a regular private file owned by the current user.");
            break;
        }
        if (operation == DisplayNameClear) {
            if (existing == 0 && unlinkat(directoryFD, "display-name", 0) != 0 && error) *error = Failure(@"Cannot clear the display name.");
        } else if (operation == DisplayNameWrite) {
            NSString *temporaryName = [@".display-name-" stringByAppendingString:NSUUID.UUID.UUIDString];
            int output = openat(directoryFD, temporaryName.UTF8String, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
            NSData *bytes = [[name stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding];
            BOOL saved = output >= 0;
            NSUInteger written = 0;
            while (saved && written < bytes.length) {
                ssize_t count = write(output, (const uint8_t *)bytes.bytes + written, bytes.length - written);
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) saved = NO;
                else written += (NSUInteger)count;
            }
            if (saved && fsync(output) != 0) saved = NO;
            if (output >= 0 && close(output) != 0) saved = NO;
            if (saved) saved = renameat(directoryFD, temporaryName.UTF8String, directoryFD, "display-name") == 0;
            if (saved) result = name;
            else {
                unlinkat(directoryFD, temporaryName.UTF8String, 0);
                if (error) *error = Failure(@"Cannot save the display name atomically.");
            }
        } else if (existing == 0) {
            int input = openat(directoryFD, "display-name", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            uint8_t bytes[kNameByteLimit + 2]; // limit plus newline, plus one overflow-detection byte
            NSUInteger length = 0;
            BOOL valid = input >= 0 && fstat(input, &attributes) == 0 && S_ISREG(attributes.st_mode) &&
                         attributes.st_uid == getuid() && attributes.st_nlink == 1 && fchmod(input, 0600) == 0;
            while (valid && length < sizeof(bytes)) {
                ssize_t count = read(input, bytes + length, sizeof(bytes) - length);
                if (count < 0 && errno == EINTR) continue;
                if (count < 0) valid = NO;
                if (count <= 0) break;
                length += (NSUInteger)count;
            }
            if (input >= 0) close(input);
            if (valid && length <= kNameByteLimit + 1 && length > 0 && bytes[length - 1] == '\n') --length;
            NSString *stored = valid && length <= kNameByteLimit ? [[NSString alloc] initWithBytes:bytes length:length encoding:NSUTF8StringEncoding] : nil;
            if (ValidDisplayName(stored, error)) result = stored;
            else if (error) *error = Failure(@"Stored display-name is invalid; use --name to replace it or --clear-name to restore LocalHostName.");
        }
    } while (NO);
    flock(lockFD, LOCK_UN);
    close(lockFD);
    close(directoryFD);
    return result;
}

static NSString *EffectiveDisplayName(NSURL *directory, NSError **error)
{
    NSError *readError = nil;
    NSString *alias = DisplayNameSetting(directory, DisplayNameRead, nil, &readError);
    if (readError) { if (error) *error = readError; return nil; }
    return alias ?: LocalMacName();
}

@interface BLESession : NSObject
@property(nonatomic, strong) CBPeripheral *peripheral;
@property(nonatomic, strong) CBCharacteristic *identity;
@property(nonatomic, strong) CBCharacteristic *quota;
@property(nonatomic) BOOL quotaPending;
@property(nonatomic, strong) NSDate *quotaDeadline;
@property(nonatomic, strong) NSDate *deadline;
@property(nonatomic) BOOL pending;
@property(nonatomic) BOOL reportedReady;
@property(nonatomic) BOOL closing;
@property(nonatomic) BOOL requestedIdentityService;
@end
@implementation BLESession
@end

@interface IdentityBridge : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
- (instancetype)initWithHostID:(NSString *)hostID directory:(NSURL *)directory codexBin:(NSString *)codexBin;
- (void)usbAttached:(IOHIDDeviceRef)device;
- (void)usbRemoved:(IOHIDDeviceRef)device;
- (void)hidAttached:(IOHIDDeviceRef)device;
- (void)hidRemoved:(IOHIDDeviceRef)device;
- (void)start;
@end

static void USBMatched(void *context, IOReturn result, void *sender, IOHIDDeviceRef device)
{
    (void)sender;
    @autoreleasepool {
        if (result == kIOReturnSuccess) [(__bridge IdentityBridge *)context hidAttached:device];
    }
}
static void USBRemoved(void *context, IOReturn result, void *sender, IOHIDDeviceRef device)
{
    (void)result; (void)sender;
    @autoreleasepool { [(__bridge IdentityBridge *)context hidRemoved:device]; }
}

@implementation IdentityBridge {
    NSString *_hostID;
    NSURL *_directory;
    CBCentralManager *_central;
    NSMutableDictionary<NSUUID *, BLESession *> *_sessions;
    NSMutableDictionary<NSValue *, NSNumber *> *_usbStates;
    IOHIDManagerRef _usbManager;
    NSTimer *_timer;
    NSString *_codexBin;
    CHPowerPolicy _power;
    NSTimeInterval _usbRetryAt;
    NSTimeInterval _connectedProbeAt;
    BOOL _tickQueued;
    NSData *_cachedPayload;
    NSData *_cachedQuota;
    NSTimeInterval _cachedQuotaAt;
    dispatch_source_t _nameWatch;
}

- (instancetype)initWithHostID:(NSString *)hostID directory:(NSURL *)directory codexBin:(NSString *)codexBin
{
    self = [super init];
    if (self) {
        _hostID = hostID;
        _directory = directory;
        _codexBin = codexBin;
        _sessions = [NSMutableDictionary dictionary];
        _usbStates = [NSMutableDictionary dictionary];
        CHPowerInit(&_power, NSProcessInfo.processInfo.systemUptime);
    }
    return self;
}

- (void)start
{
    [self watchDisplayName];
    [self reloadPayload];
    [NSWorkspace.sharedWorkspace.notificationCenter addObserver:self selector:@selector(didWake:)
                                                          name:NSWorkspaceDidWakeNotification object:nil];
    _central = [[CBCentralManager alloc] initWithDelegate:self queue:dispatch_get_main_queue()];
    _usbManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    NSDictionary *matching = @{@kIOHIDVendorIDKey: @(kVendorID), @kIOHIDProductIDKey: @(kProductID),
                               @kIOHIDPrimaryUsagePageKey: @0xFF00,
                               @kIOHIDPrimaryUsageKey: @1};
    IOHIDManagerSetDeviceMatching(_usbManager, (__bridge CFDictionaryRef)matching);
    IOHIDManagerRegisterDeviceMatchingCallback(_usbManager, USBMatched, (__bridge void *)self);
    IOHIDManagerRegisterDeviceRemovalCallback(_usbManager, USBRemoved, (__bridge void *)self);
    IOHIDManagerScheduleWithRunLoop(_usbManager, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    // Watch native USB and BLE HID appearance without seizing or reading reports.
    // Only a device whose transport is exactly USB receives Feature 7/8 writes.
    IOReturn status = IOHIDManagerOpen(_usbManager, kIOHIDOptionsTypeNone);
    if (status != kIOReturnSuccess) fprintf(stderr, "[USB] Manager open failed: 0x%08x\n", status);
    [self requestTick];
    puts("[Identity] Waiting for compatible StopWatch firmware; no control events are generated.");
}

- (void)reloadPayload
{
    NSError *error = nil;
    NSString *name = EffectiveDisplayName(_directory, &error);
    _cachedPayload = name ? IdentityData(_hostID, name, &error) : nil;
    if (!_cachedPayload) fprintf(stderr, "[Identity] %s\n", error.localizedDescription.UTF8String);
}

- (NSData *)payload { return _cachedPayload; }

- (void)watchDisplayName
{
    if (_nameWatch) return;
    int fd = open(_directory.fileSystemRepresentation, O_EVTONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return; // Recovery tick retries if the directory was replaced.
    _nameWatch = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, (uintptr_t)fd,
                                        DISPATCH_VNODE_WRITE | DISPATCH_VNODE_RENAME | DISPATCH_VNODE_DELETE,
                                        dispatch_get_main_queue());
    if (!_nameWatch) { close(fd); return; }
    dispatch_source_set_cancel_handler(_nameWatch, ^{ close(fd); });
    dispatch_source_set_event_handler(_nameWatch, ^{
        @autoreleasepool {
            unsigned long events = dispatch_source_get_data(self->_nameWatch);
            if (events & (DISPATCH_VNODE_RENAME | DISPATCH_VNODE_DELETE)) {
                dispatch_source_cancel(self->_nameWatch);
                self->_nameWatch = nil;
            }
            NSData *previous = self->_cachedPayload;
            [self reloadPayload];
            if (![previous isEqual:self->_cachedPayload]) {
                CHPowerRequestIdentity(&self->_power, NSProcessInfo.processInfo.systemUptime);
                [self requestTick];
            }
            // File permissions/lock reads do not change directory entries, so
            // they cannot recursively trigger this WRITE-only contents watch.
        }
    });
    dispatch_resume(_nameWatch);
}

- (void)didWake:(NSNotification *)notification
{
    (void)notification;
    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            CHPowerWake(&self->_power, NSProcessInfo.processInfo.systemUptime);
            [self reloadPayload];
            [self requestTick];
        }
    });
}

- (void)hidAttached:(IOHIDDeviceRef)device
{
    NSString *transport = (__bridge NSString *)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDTransportKey));
    if ([transport isEqual:@"USB"]) [self usbAttached:device];
    else {
        // CBCentralManager connection-event registration is unavailable on
        // macOS. Native BLE HID arrival provides an event without radio scans.
        [self findConnected];
        _connectedProbeAt = NSProcessInfo.processInfo.systemUptime + 2;
        [self requestTick];
    }
}

- (void)hidRemoved:(IOHIDDeviceRef)device
{
    NSString *transport = (__bridge NSString *)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDTransportKey));
    if ([transport isEqual:@"USB"]) [self usbRemoved:device];
    else {
        CHPowerDiscoveryLost(&_power, NSProcessInfo.processInfo.systemUptime);
        [self requestTick];
    }
}

- (void)usbAttached:(IOHIDDeviceRef)device
{
    NSDictionary *match = @{@kIOHIDElementReportIDKey: @(kIdentityReportID),
                           @kIOHIDElementTypeKey: @(kIOHIDElementTypeFeature)};
    NSArray *elements = CFBridgingRelease(IOHIDDeviceCopyMatchingElements(device,
                                         (__bridge CFDictionaryRef)match, kIOHIDOptionsTypeNone));
    if (elements.count == 0) {
        puts("[USB] Matching device lacks identity feature report 7; firmware update required.");
        return;
    }
    NSValue *key = [NSValue valueWithPointer:device];
    if (_usbStates[key]) return;
    CFRetain(device);
    _usbStates[key] = @NO;
    [self sendUSB:device payload:[self payload]];
    [self findConnected]; // Keep the same Mac's already-connected BLE fallback identified.
    [self requestTick];
}

- (void)usbRemoved:(IOHIDDeviceRef)device
{
    NSValue *key = [NSValue valueWithPointer:device];
    if (!_usbStates[key]) return;
    [_usbStates removeObjectForKey:key];
    CFRelease(device);
    puts("[USB] StopWatch detached.");
    CHPowerDiscoveryLost(&_power, NSProcessInfo.processInfo.systemUptime);
    [self findConnected];
    [self requestTick];
}

- (void)sendUSB:(IOHIDDeviceRef)device payload:(NSData *)data
{
    if (!data || data.length >= kIdentityBodySize) return;
    uint8_t report[1 + kIdentityBodySize] = {kIdentityReportID};
    memcpy(report + 1, data.bytes, data.length);
    IOReturn status = IOHIDDeviceSetReport(device, kIOHIDReportTypeFeature, kIdentityReportID,
                                         report, sizeof(report));
    NSValue *key = [NSValue valueWithPointer:device];
    BOOL wasReady = _usbStates[key].integerValue == 1;
    BOOL ready = status == kIOReturnSuccess;
    if (ready && !wasReady) puts("[USB] Identity feature report delivered; firmware decides the active host.");
    if (!ready && (wasReady || _usbStates[key].integerValue == 0)) {
        fprintf(stderr, "[USB] Identity report failed: 0x%08x (retrying every 5 seconds).\n", status);
    }
    _usbStates[key] = ready ? @1 : @(-1);
    if (ready && !wasReady) [self quotaPathReady];
    if (!ready) _usbRetryAt = NSProcessInfo.processInfo.systemUptime + CHUSBRetrySeconds;
}

- (CHPowerInputs)powerInputs
{
    CHPowerInputs inputs = {.bluetoothOn = _central.state == CBManagerStatePoweredOn,
                            .usbReady = [self hasUSBIdentity]};
    BOOL quotaReady = inputs.usbReady;
    for (BLESession *session in _sessions.allValues) {
        if (!session.closing) {
            inputs.bleBusy = YES;
            if (session.reportedReady && session.quota) quotaReady = YES;
        }
    }
    inputs.quotaReady = _codexBin != nil && quotaReady;
    return inputs;
}

- (void)requestTick
{
    if (_tickQueued) return;
    _tickQueued = YES;
    dispatch_async(dispatch_get_main_queue(), ^{
        self->_tickQueued = NO;
        [self tick:nil];
    });
}

- (void)scheduleTick
{
    [_timer invalidate];
    NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
    double deadline = CHPowerNextDeadline(&_power, [self powerInputs]);
    if (_usbRetryAt > 0) deadline = fmin(deadline, _usbRetryAt);
    if (_connectedProbeAt > 0) deadline = fmin(deadline, _connectedProbeAt);
    for (BLESession *session in _sessions.allValues) {
        if (session.closing) continue;
        if (session.deadline) deadline = fmin(deadline, now + session.deadline.timeIntervalSinceNow);
        if (session.quotaDeadline) deadline = fmin(deadline, now + session.quotaDeadline.timeIntervalSinceNow);
    }
    NSTimeInterval delay = fmax(0.01, deadline - now);
    _timer = [NSTimer scheduledTimerWithTimeInterval:delay target:self selector:@selector(tick:)
                                           userInfo:nil repeats:NO];
    _timer.tolerance = fmin(0.5, delay * 0.1);
}

- (void)tick:(NSTimer *)timer
{
    (void)timer;
    @autoreleasepool {
        NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
        for (BLESession *session in _sessions.allValues) {
            if (((session.deadline && session.deadline.timeIntervalSinceNow <= 0) ||
                 (session.quotaDeadline && session.quotaDeadline.timeIntervalSinceNow <= 0)) && !session.closing) {
                fprintf(stderr, "[BLE] Identity handshake timeout for %s.\n", session.peripheral.identifier.UUIDString.UTF8String);
                [self closeSession:session];
            }
        }
        if (now >= _power.nextRecoveryAt || (_connectedProbeAt > 0 && now >= _connectedProbeAt)) {
            _connectedProbeAt = 0;
            [self findConnected]; // Also works while USB is active; never requires a radio scan.
            if (!_nameWatch) { [self reloadPayload]; [self watchDisplayName]; }
        }
        CHPowerActions actions = CHPowerEvaluate(&_power, now, [self powerInputs]);
        if (actions.stopScan && _central.isScanning) [_central stopScan];
        if (actions.startScan) [_central scanForPeripheralsWithServices:@[ServiceUUID()]
                                       options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @NO}];
        BOOL retryUSB = _usbRetryAt > 0 && now >= _usbRetryAt;
        if (actions.recoverIdentity || retryUSB) {
            NSData *data = [self payload];
            for (NSValue *key in _usbStates.allKeys) {
                if (actions.recoverIdentity || _usbStates[key].integerValue != 1)
                    [self sendUSB:(IOHIDDeviceRef)key.pointerValue payload:data];
            }
            if (actions.recoverIdentity) {
                for (BLESession *session in _sessions.allValues)
                    if (session.identity) [self sendBLE:session payload:data];
            }
        }
        BOOL failedUSB = NO;
        for (NSNumber *state in _usbStates.allValues) if (state.integerValue != 1) failedUSB = YES;
        if (!failedUSB) _usbRetryAt = 0;
        else if (_usbRetryAt <= now) _usbRetryAt = now + CHUSBRetrySeconds;
        if (actions.readQuota) [self readQuota];
        [self scheduleTick];
    }
}

- (void)findConnected
{
    if (_central.state != CBManagerStatePoweredOn) return;
    for (CBPeripheral *peripheral in [_central retrieveConnectedPeripheralsWithServices:@[ServiceUUID()]]) {
        [self connect:peripheral];
    }
}

- (BOOL)hasUSBIdentity
{
    for (NSNumber *state in _usbStates.allValues) if (state.integerValue == 1) return YES;
    return NO;
}

- (void)quotaPathReady
{
    NSData *recent = RecentQuota(_cachedQuota, NSDate.date.timeIntervalSince1970 - _cachedQuotaAt);
    if (recent) [self deliverQuota:recent];
    CHPowerNewQuotaPath(&_power, NSProcessInfo.processInfo.systemUptime);
}

- (void)readQuota
{
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            NSError *error = nil;
            NSData *data = ReadCodexQuota(self->_codexBin, &error);
            dispatch_async(dispatch_get_main_queue(), ^{
                @autoreleasepool {
                    CHPowerQuotaFinished(&self->_power);
                    if (!data) fprintf(stderr, "[Quota] %s (next attempt in 180 seconds).\n", error.localizedDescription.UTF8String);
                    else {
                        self->_cachedQuota = data;
                        self->_cachedQuotaAt = NSDate.date.timeIntervalSince1970;
                        [self deliverQuota:data];
                    }
                    [self requestTick];
                }
            });
        }
    });
}

- (void)deliverQuota:(NSData *)data
{
    if ([self hasUSBIdentity]) {
        if (data.length >= 256) return;
        uint8_t report[257] = {8};
        memcpy(report + 1, data.bytes, data.length);
        for (NSValue *key in _usbStates.allKeys) {
            if (_usbStates[key].integerValue != 1) continue;
            IOReturn status = IOHIDDeviceSetReport((IOHIDDeviceRef)key.pointerValue, kIOHIDReportTypeFeature, 8,
                                                 report, sizeof(report));
            if (status != kIOReturnSuccess) fprintf(stderr, "[USB] Quota report failed: 0x%08x.\n", status);
        }
        return;
    }
    for (BLESession *session in _sessions.allValues) {
        if (session.reportedReady && !session.closing && session.quota && !session.quotaPending &&
            data.length <= [session.peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithResponse]) {
            session.quotaPending = YES;
            session.quotaDeadline = [NSDate dateWithTimeIntervalSinceNow:30];
            [session.peripheral writeValue:data forCharacteristic:session.quota type:CBCharacteristicWriteWithResponse];
        }
    }
}

- (void)connect:(CBPeripheral *)peripheral
{
    if (_sessions[peripheral.identifier] || _central.state != CBManagerStatePoweredOn) return;
    BLESession *session = [BLESession new];
    session.peripheral = peripheral;
    session.deadline = [NSDate dateWithTimeIntervalSinceNow:CHHandshakeSeconds];
    _sessions[peripheral.identifier] = session;
    peripheral.delegate = self;
    [_central connectPeripheral:peripheral options:nil];
    [self requestTick];
}

- (void)closeSession:(BLESession *)session
{
    session.closing = YES;
    if (_central.isScanning) [_central stopScan];
    CHPowerDiscoveryFailed(&_power, NSProcessInfo.processInfo.systemUptime);
    [_central cancelPeripheralConnection:session.peripheral];
    [self requestTick];
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
    if (central.state == CBManagerStatePoweredOn) {
        CHPowerDiscoveryLost(&_power, NSProcessInfo.processInfo.systemUptime);
        [self findConnected];
    } else {
        [_sessions removeAllObjects];
        if (central.state == CBManagerStateUnauthorized) {
            puts("[BLE] Permission denied. Allow Codex Host Identity in System Settings > Privacy & Security > Bluetooth.");
        } else if (central.state == CBManagerStatePoweredOff) {
            puts("[BLE] Bluetooth is off; USB discovery remains available.");
        } else if (central.state == CBManagerStateUnsupported) {
            puts("[BLE] Bluetooth is unsupported on this Mac; USB discovery remains available.");
        }
    }
    [self requestTick];
}

- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)peripheral
     advertisementData:(NSDictionary<NSString *, id> *)advertisementData RSSI:(NSNumber *)RSSI
{
    (void)central; (void)advertisementData; (void)RSSI;
    [self connect:peripheral];
}

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)peripheral
{
    (void)central;
    // Rediscover the service database after firmware upgrades (macOS caches GATT).
    [peripheral discoverServices:nil];
    [self requestTick];
}

- (void)centralManager:(CBCentralManager *)central didFailToConnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
    (void)central;
    fprintf(stderr, "[BLE] Connection failed: %s\n", error.localizedDescription.UTF8String ?: "unknown error");
    [_sessions removeObjectForKey:peripheral.identifier];
    if (_central.isScanning) [_central stopScan];
    CHPowerDiscoveryFailed(&_power, NSProcessInfo.processInfo.systemUptime);
    [self requestTick];
}

- (void)centralManager:(CBCentralManager *)central didDisconnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
    (void)central;
    fprintf(stderr, "[BLE] Disconnected: %s\n", error.localizedDescription.UTF8String ?: "connection closed");
    BOOL recoveringFailure = _sessions[peripheral.identifier].closing;
    [_sessions removeObjectForKey:peripheral.identifier];
    if (!recoveringFailure) CHPowerDiscoveryLost(&_power, NSProcessInfo.processInfo.systemUptime);
    [self requestTick];
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error
{
    BLESession *session = _sessions[peripheral.identifier];
    if (!session || session.closing) return;
    if (error) { fprintf(stderr, "[BLE] Service discovery: %s\n", error.localizedDescription.UTF8String); [self closeSession:session]; return; }
    CBService *identityService = nil, *quotaService = nil;
    for (CBService *service in peripheral.services) {
        if ([service.UUID isEqual:IdentityServiceUUID()]) identityService = service;
        if ([service.UUID isEqual:ServiceUUID()]) quotaService = service;
    }
    if (!identityService && !session.requestedIdentityService) {
        // The identity service is appended after the five original services.
        // Ask for it explicitly if an initial discovery returned a cached list.
        session.requestedIdentityService = YES;
        [peripheral discoverServices:@[IdentityServiceUUID(), ServiceUUID()]];
        return;
    }
    if (!identityService) {
        puts("[BLE] Appended identity service unavailable; waiting for compatible firmware/discovery.");
        [self closeSession:session];
        return;
    }
    [peripheral discoverCharacteristics:@[IdentityUUID()] forService:identityService];
    if (quotaService) [peripheral discoverCharacteristics:@[QuotaUUID()] forService:quotaService];
    else puts("[BLE] Quota service unavailable; host identity remains usable.");
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverCharacteristicsForService:(CBService *)service error:(NSError *)error
{
    BLESession *session = _sessions[peripheral.identifier];
    if (!session || session.closing) return;
    if (error) { fprintf(stderr, "[BLE] Characteristic discovery: %s\n", error.localizedDescription.UTF8String); [self closeSession:session]; return; }
    if ([service.UUID isEqual:IdentityServiceUUID()]) {
        for (CBCharacteristic *characteristic in service.characteristics) {
            if ([characteristic.UUID isEqual:IdentityUUID()] && (characteristic.properties & CBCharacteristicPropertyWrite)) {
                session.identity = characteristic;
            }
        }
        if (session.identity) { [self sendBLE:session payload:[self payload]]; return; }
        puts("[BLE] Identity characteristic absent from the appended service; firmware update required.");
        [self closeSession:session];
    } else if ([service.UUID isEqual:ServiceUUID()]) {
        for (CBCharacteristic *characteristic in service.characteristics) {
            if ([characteristic.UUID isEqual:QuotaUUID()] && (characteristic.properties & CBCharacteristicPropertyWrite)) {
                session.quota = characteristic;
            }
        }
        if (!session.quota) puts("[BLE] Quota characteristic unavailable; host identity remains usable.");
        else if (session.reportedReady) [self quotaPathReady];
        [self requestTick];
    }
}

- (void)sendBLE:(BLESession *)session payload:(NSData *)data
{
    if (!data || session.pending || session.closing) return;
    if (data.length > [session.peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithResponse]) {
        puts("[BLE] Identity exceeds negotiated write limit; reconnecting instead of sending partial JSON.");
        [self closeSession:session];
        return;
    }
    session.pending = YES;
    session.deadline = [NSDate dateWithTimeIntervalSinceNow:CHHandshakeSeconds];
    // Firmware requires encryption. CoreBluetooth negotiates pairing as needed.
    [session.peripheral writeValue:data forCharacteristic:session.identity type:CBCharacteristicWriteWithResponse];
    [self requestTick];
}

- (void)peripheral:(CBPeripheral *)peripheral didWriteValueForCharacteristic:(CBCharacteristic *)characteristic error:(NSError *)error
{
    BLESession *session = _sessions[peripheral.identifier];
    if (!session) return;
    if ([characteristic.UUID isEqual:QuotaUUID()]) {
        session.quotaPending = NO;
        session.quotaDeadline = nil;
        if (error) fprintf(stderr, "[BLE] Quota write failed: %s\n", error.localizedDescription.UTF8String);
        [self requestTick];
        return;
    }
    if (![characteristic.UUID isEqual:IdentityUUID()]) return;
    session.pending = NO;
    if (error) {
        fprintf(stderr, "[BLE] Identity write failed: %s\n", error.localizedDescription.UTF8String);
        [self closeSession:session];
        return;
    }
    session.deadline = nil;
    if (!session.reportedReady) {
        puts("[BLE] Identity accepted on encrypted characteristic; firmware decides the active host.");
        CHPowerDiscoveryLost(&_power, NSProcessInfo.processInfo.systemUptime);
        session.reportedReady = YES;
        [self quotaPathReady];
    }
    [self requestTick];
}
@end

static BOOL TestDisplayNames(NSUInteger *passed)
{
    NSURL *temp = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString]
                            isDirectory:YES];
    NSFileManager *fm = NSFileManager.defaultManager;
    NSURL *nameURL = [temp URLByAppendingPathComponent:@"display-name"];
    NSURL *idURL = [temp URLByAppendingPathComponent:@"host-id"];
    NSError *error = nil;
    NSUInteger checks = 0;
#define CHECK_NAME(condition) do { if (!(condition)) { fprintf(stderr, "Self-test: display-name case %lu failed.\n", (unsigned long)checks); return NO; } ++checks; } while (NO)
    @try {
        CHECK_NAME(!DisplayNameSetting(temp, DisplayNameRead, nil, &error) && !error && ![fm fileExistsAtPath:idURL.path]);
        CHECK_NAME([DisplayNameSetting(temp, DisplayNameWrite, @"Mac P", &error) isEqual:@"Mac P"] && !error);
        CHECK_NAME([DisplayNameSetting(temp, DisplayNameRead, nil, &error) isEqual:@"Mac P"] && !error && ![fm fileExistsAtPath:idURL.path]);
        struct stat attributes;
        CHECK_NAME(stat(nameURL.fileSystemRepresentation, &attributes) == 0 && (attributes.st_mode & 0777) == 0600 &&
                   stat(temp.fileSystemRepresentation, &attributes) == 0 && (attributes.st_mode & 0777) == 0700);
        NSString *hostID = LoadIdentity(temp, &error);
        NSData *originalIDFile = [NSData dataWithContentsOfURL:idURL];
        CHECK_NAME(hostID && !error && originalIDFile);
        CHECK_NAME([DisplayNameSetting(temp, DisplayNameWrite, @"Mac W", &error) isEqual:@"Mac W"] && !error &&
                   [EffectiveDisplayName(temp, &error) isEqual:@"Mac W"] && [[NSData dataWithContentsOfURL:idURL] isEqual:originalIDFile]);
        NSArray *invalid = @[@"", @" ", @" Mac P", @"Mac P ", @"Mac\nP", @"Mac\tP",
                             [@"a" stringByPaddingToLength:32 withString:@"a" startingAtIndex:0],
                             [@"测" stringByPaddingToLength:11 withString:@"测" startingAtIndex:0],
                             [@"\"" stringByPaddingToLength:31 withString:@"\"" startingAtIndex:0]];
        for (NSString *name in invalid) {
            error = nil;
            CHECK_NAME(!DisplayNameSetting(temp, DisplayNameWrite, name, &error) && error);
            error = nil;
            CHECK_NAME([DisplayNameSetting(temp, DisplayNameRead, nil, &error) isEqual:@"Mac W"] && !error);
        }
        for (NSString *name in @[@"私人 Mac", [@"a" stringByPaddingToLength:31 withString:@"a" startingAtIndex:0]]) {
            error = nil;
            CHECK_NAME([DisplayNameSetting(temp, DisplayNameWrite, name, &error) isEqual:name] && !error &&
                       [DisplayNameSetting(temp, DisplayNameRead, nil, &error) isEqual:name]);
        }
        const uint8_t corrupt[] = {0xff, 0xfe, '\n'};
        CHECK_NAME([[NSData dataWithBytes:corrupt length:sizeof(corrupt)] writeToURL:nameURL options:0 error:&error]);
        error = nil;
        CHECK_NAME(!EffectiveDisplayName(temp, &error) && error);
        error = nil;
        CHECK_NAME([DisplayNameSetting(temp, DisplayNameWrite, @"Mac P", &error) isEqual:@"Mac P"] && !error);
        CHECK_NAME(!DisplayNameSetting(temp, DisplayNameClear, nil, &error) && !error && ![fm fileExistsAtPath:nameURL.path] &&
                   [EffectiveDisplayName(temp, &error) isEqual:LocalMacName()] && [[NSData dataWithContentsOfURL:idURL] isEqual:originalIDFile]);
        CHECK_NAME(!DisplayNameSetting(temp, DisplayNameClear, nil, &error) && !error);
        CHECK_NAME(symlink(idURL.fileSystemRepresentation, nameURL.fileSystemRepresentation) == 0);
        for (NSNumber *operation in @[@(DisplayNameRead), @(DisplayNameWrite), @(DisplayNameClear)]) {
            error = nil;
            CHECK_NAME(!DisplayNameSetting(temp, operation.unsignedIntegerValue, @"Mac W", &error) && error &&
                       [[NSData dataWithContentsOfURL:idURL] isEqual:originalIDFile]);
        }
        CHECK_NAME(unlink(nameURL.fileSystemRepresentation) == 0 && link(idURL.fileSystemRepresentation, nameURL.fileSystemRepresentation) == 0);
        error = nil;
        CHECK_NAME(!DisplayNameSetting(temp, DisplayNameRead, nil, &error) && error && [[NSData dataWithContentsOfURL:idURL] isEqual:originalIDFile]);
        CHECK_NAME(unlink(nameURL.fileSystemRepresentation) == 0);
        NSString *oversized = [@"a" stringByPaddingToLength:4096 withString:@"a" startingAtIndex:0];
        error = nil;
        CHECK_NAME([oversized writeToURL:nameURL atomically:YES encoding:NSUTF8StringEncoding error:&error]);
        CHECK_NAME(!DisplayNameSetting(temp, DisplayNameRead, nil, &error) && error);
        *passed = checks;
        return YES;
    } @finally {
        [fm removeItemAtURL:temp error:nil];
    }
#undef CHECK_NAME
}

static int SelfTest(void)
{
    NSString *hostID = @"5c5b1aad-3f81-4d87-9033-7e08be65e4a8";
    NSArray *names = @[@"Ryan-MacBook", @"", @"测试电脑💻测试电脑💻测试电脑💻", @"a\n\tb",
                      [@"\\\"" stringByPaddingToLength:120 withString:@"\\\"" startingAtIndex:0],
                      @"👩🏽‍💻👩🏽‍💻👩🏽‍💻👩🏽‍💻"];
    NSUInteger checks = 0;
    for (NSString *name in names) {
        NSError *error = nil;
        NSData *data = IdentityData(hostID, name, &error);
        NSDictionary *decoded = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:&error] : nil;
        if (!data || error || data.length >= kIdentityBodySize || ![decoded[@"hostId"] isEqual:hostID] ||
            ![decoded[@"version"] isEqual:@1] || [decoded[@"name"] lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > kNameByteLimit ||
            ![decoded[@"name"] length]) { fprintf(stderr, "Self-test: identity encoding failed at case %lu: %s (bytes %lu).\n", (unsigned long)checks, error.localizedDescription.UTF8String ?: "no error", (unsigned long)data.length); return 1; }
        uint8_t usb[1 + kIdentityBodySize] = {kIdentityReportID};
        memcpy(usb + 1, data.bytes, data.length);
        if (usb[0] != 7 || memcmp(usb + 1, data.bytes, data.length) != 0 || usb[data.length + 1] != 0) return 1;
        ++checks;
    }
    NSError *error = nil;
    if (IdentityData(@"bad-id", @"Mac", &error) != nil || !error) return 1;
    ++checks;
    NSURL *temp = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString]
                            isDirectory:YES];
    error = nil;
    NSString *first = LoadIdentity(temp, &error);
    NSString *second = LoadIdentity(temp, &error);
    NSURL *idURL = [temp URLByAppendingPathComponent:@"host-id"];
    struct stat attributes;
    BOOL protected = stat(idURL.fileSystemRepresentation, &attributes) == 0 && (attributes.st_mode & 0777) == 0600;
    BOOL okay = first && !error && [first isEqual:second] && protected;
    ++checks;
    [@"corrupt\n" writeToURL:idURL atomically:YES encoding:NSUTF8StringEncoding error:nil];
    error = nil;
    okay = okay && !LoadIdentity(temp, &error) && error != nil;
    ++checks;
    [NSFileManager.defaultManager removeItemAtURL:temp error:nil];
    if (!okay) { fprintf(stderr, "Self-test: persistent identity validation failed.\n"); return 1; }
    if (!TestQuotaEncoding()) { fprintf(stderr, "Self-test: quota encoding failed.\n"); return 1; }
    checks += 5;
    NSData *quota = [NSJSONSerialization dataWithJSONObject:@{@"version": @1, @"remaining_percent": @50, @"reset_in_seconds": @10}
                                                  options:0 error:nil];
    NSDictionary *aged = [NSJSONSerialization JSONObjectWithData:RecentQuota(quota, 2.5) options:0 error:nil];
    if (![aged[@"reset_in_seconds"] isEqual:@7] || ![aged[@"remaining_percent"] isEqual:@50]) return 1;
    ++checks;
    aged = [NSJSONSerialization JSONObjectWithData:RecentQuota(quota, 15) options:0 error:nil];
    if (![aged[@"reset_in_seconds"] isEqual:@0]) return 1;
    ++checks;
    if (RecentQuota(quota, CHQuotaMinimumIntervalSeconds) != nil) return 1;
    ++checks;
    if (RecentQuota(quota, -1) != nil || RecentQuota(quota, NAN) != nil) return 1;
    ++checks;
    NSUInteger nameChecks = 0;
    if (!TestDisplayNames(&nameChecks)) return 1;
    checks += nameChecks;
    printf("{\"selfTest\":\"passed\",\"checks\":%lu,\"hardwareAccessed\":false}\n", (unsigned long)checks);
    return 0;
}

static void Usage(void)
{
    puts("Usage: codex-host-identity [--help | --self-test | --show-identity | --show-name | --name NAME | --clear-name]\n"
         "       codex-host-identity [--codex-bin PATH | --no-quota]\n"
         "  No arguments: announce this Mac over BLE/USB and sync Codex quota.\n"
         "  --show-identity  Print/create the local identity without accessing hardware.\n"
         "  --show-name      Print the effective display name and its source, then exit.\n"
         "  --name NAME      Save a display alias (for example 'Mac P'), then exit.\n"
         "  --clear-name     Remove the alias and restore LocalHostName, then exit.\n"
         "  Name commands never create/change the UUID or access hardware.\n"
         "  --self-test      Validate encoding and temporary-file persistence offline.\n"
         "  --codex-bin PATH Use this absolute Codex executable for read-only quota calls.\n"
         "  --no-quota       Announce identity without starting a Codex app-server.\n"
         "Launch the app bundle with open so macOS can associate Bluetooth permission.\n"
         "This helper does not send Codex controls or read input reports.");
}

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        BOOL showIdentity = NO, showName = NO, clearName = NO, quotaEnabled = YES, runtimeOptions = NO;
        NSUInteger offlineActions = 0;
        NSString *codexBin = nil, *requestedName = nil;
        for (int index = 1; index < argc; ++index) {
            if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0) { Usage(); return 0; }
            if (strcmp(argv[index], "--self-test") == 0 && argc == 2) return SelfTest();
            if (strcmp(argv[index], "--show-identity") == 0) { showIdentity = YES; ++offlineActions; }
            else if (strcmp(argv[index], "--show-name") == 0) { showName = YES; ++offlineActions; }
            else if (strcmp(argv[index], "--clear-name") == 0) { clearName = YES; ++offlineActions; }
            else if (strcmp(argv[index], "--name") == 0 && index + 1 < argc) {
                requestedName = [NSString stringWithUTF8String:argv[++index]];
                if (!requestedName) { fprintf(stderr, "--name requires valid UTF-8.\n"); return 2; }
                ++offlineActions;
            }
            else if (strcmp(argv[index], "--no-quota") == 0) { quotaEnabled = NO; runtimeOptions = YES; }
            else if (strcmp(argv[index], "--codex-bin") == 0 && index + 1 < argc) {
                runtimeOptions = YES;
                codexBin = [NSString stringWithUTF8String:argv[++index]];
                if (!codexBin.isAbsolutePath) { fprintf(stderr, "--codex-bin requires an absolute path.\n"); return 2; }
            } else { Usage(); return 2; }
        }
        if (offlineActions > 1 || (offlineActions && runtimeOptions)) {
            fprintf(stderr, "Use one offline command at a time, without startup options.\n");
            return 2;
        }
        NSURL *support = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
        NSURL *directory = [support URLByAppendingPathComponent:@"Friday/Codex Host Identity" isDirectory:YES];
        NSError *error = nil;
        if (showName || clearName || requestedName) {
            DisplayNameOperation operation = requestedName ? DisplayNameWrite : clearName ? DisplayNameClear : DisplayNameRead;
            NSString *alias = DisplayNameSetting(directory, operation, requestedName, &error);
            if (error) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
            NSData *identity = IdentityData(@"5c5b1aad-3f81-4d87-9033-7e08be65e4a8", alias ?: LocalMacName(), &error);
            NSDictionary *decoded = identity ? [NSJSONSerialization JSONObjectWithData:identity options:0 error:&error] : nil;
            NSData *details = decoded ? [NSJSONSerialization dataWithJSONObject:@{@"name": decoded[@"name"], @"source": alias ? @"alias" : @"LocalHostName"}
                                                                        options:NSJSONWritingSortedKeys error:&error] : nil;
            if (!details) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
            puts([[NSString alloc] initWithData:details encoding:NSUTF8StringEncoding].UTF8String);
            return 0;
        }
        NSString *hostID = LoadIdentity(directory, &error);
        if (!hostID) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
        if (showIdentity) {
            NSString *name = EffectiveDisplayName(directory, &error);
            NSData *data = name ? IdentityData(hostID, name, &error) : nil;
            if (!data) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
            puts([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding].UTF8String);
            return 0;
        }
        NSURL *runLock = [directory URLByAppendingPathComponent:@"running.lock"];
        int runFD = open(runLock.fileSystemRepresentation, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (runFD < 0 || flock(runFD, LOCK_EX | LOCK_NB) != 0) {
            if (runFD >= 0) close(runFD);
            fprintf(stderr, "Another Codex Host Identity process is running, or the run lock is unavailable.\n");
            return 1;
        }
        if (quotaEnabled && !codexBin) codexBin = FindCodexExecutable();
        if (quotaEnabled && !codexBin) fprintf(stderr, "[Quota] No Codex executable found; identity continues. Use --codex-bin to enable quota.\n");
        [NSApplication.sharedApplication setActivationPolicy:NSApplicationActivationPolicyProhibited];
        IdentityBridge *bridge = [[IdentityBridge alloc] initWithHostID:hostID directory:directory codexBin:quotaEnabled ? codexBin : nil];
        [bridge start];
        [NSRunLoop.mainRunLoop run];
        close(runFD);
    }
    return 0;
}
