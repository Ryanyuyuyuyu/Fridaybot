// SPDX-License-Identifier: MIT
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import "quota_reader.h"
#import <CoreBluetooth/CoreBluetooth.h>
#import <IOKit/hid/IOHIDManager.h>
#import <SystemConfiguration/SystemConfiguration.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static const NSInteger kVendorID = 0x303A;
static const NSInteger kProductID = 0x8360;
static const uint8_t kIdentityReportID = 7;
enum { kIdentityBodySize = 128 };
static const NSUInteger kNameByteLimit = 31;

static CBUUID *ServiceUUID(void) { return [CBUUID UUIDWithString:@"7F0D4E66-2AC2-4A71-BFBE-4EF61A0E5C01"]; }
static CBUUID *QuotaUUID(void) { return [CBUUID UUIDWithString:@"7F0D4E66-2AC2-4A71-BFBE-4EF61A0E5C02"]; }
static CBUUID *IdentityUUID(void) { return [CBUUID UUIDWithString:@"7F0D4E66-2AC2-4A71-BFBE-4EF61A0E5C03"]; }

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

static NSString *LoadIdentity(NSURL *directory, NSError **error)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm createDirectoryAtURL:directory withIntermediateDirectories:YES
                       attributes:@{NSFilePosixPermissions: @0700} error:error]) return nil;
    struct stat directoryAttributes;
    if (lstat(directory.fileSystemRepresentation, &directoryAttributes) != 0 ||
        !S_ISDIR(directoryAttributes.st_mode) || directoryAttributes.st_uid != getuid() ||
        chmod(directory.fileSystemRepresentation, 0700) != 0) {
        if (error) *error = Failure(@"The identity directory must be a private directory owned by the current user.");
        return nil;
    }
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
@end
@implementation BLESession
@end

@interface IdentityBridge : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
- (instancetype)initWithHostID:(NSString *)hostID codexBin:(NSString *)codexBin;
- (void)usbAttached:(IOHIDDeviceRef)device;
- (void)usbRemoved:(IOHIDDeviceRef)device;
- (void)start;
@end

static void USBMatched(void *context, IOReturn result, void *sender, IOHIDDeviceRef device)
{
    (void)sender;
    if (result == kIOReturnSuccess) [(__bridge IdentityBridge *)context usbAttached:device];
}
static void USBRemoved(void *context, IOReturn result, void *sender, IOHIDDeviceRef device)
{
    (void)result; (void)sender;
    [(__bridge IdentityBridge *)context usbRemoved:device];
}

@implementation IdentityBridge {
    NSString *_hostID;
    CBCentralManager *_central;
    NSMutableDictionary<NSUUID *, BLESession *> *_sessions;
    NSMutableDictionary<NSUUID *, NSDate *> *_retryAfter;
    NSMutableDictionary<NSValue *, NSNumber *> *_usbStates;
    IOHIDManagerRef _usbManager;
    NSTimer *_timer;
    NSString *_codexBin;
    BOOL _quotaReading;
    NSTimeInterval _nextQuotaRead;
}

- (instancetype)initWithHostID:(NSString *)hostID codexBin:(NSString *)codexBin
{
    self = [super init];
    if (self) {
        _hostID = hostID;
        _codexBin = codexBin;
        _sessions = [NSMutableDictionary dictionary];
        _retryAfter = [NSMutableDictionary dictionary];
        _usbStates = [NSMutableDictionary dictionary];
    }
    return self;
}

- (void)start
{
    _central = [[CBCentralManager alloc] initWithDelegate:self queue:dispatch_get_main_queue()];
    _usbManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    NSDictionary *matching = @{@kIOHIDVendorIDKey: @(kVendorID), @kIOHIDProductIDKey: @(kProductID),
                               @kIOHIDTransportKey: @"USB", @kIOHIDPrimaryUsagePageKey: @0xFF00,
                               @kIOHIDPrimaryUsageKey: @1};
    IOHIDManagerSetDeviceMatching(_usbManager, (__bridge CFDictionaryRef)matching);
    IOHIDManagerRegisterDeviceMatchingCallback(_usbManager, USBMatched, (__bridge void *)self);
    IOHIDManagerRegisterDeviceRemovalCallback(_usbManager, USBRemoved, (__bridge void *)self);
    IOHIDManagerScheduleWithRunLoop(_usbManager, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    // Never seize the HID device: Codex must retain its report-6 connection.
    IOReturn status = IOHIDManagerOpen(_usbManager, kIOHIDOptionsTypeNone);
    if (status != kIOReturnSuccess) fprintf(stderr, "[USB] Manager open failed: 0x%08x\n", status);
    _timer = [NSTimer scheduledTimerWithTimeInterval:5 target:self selector:@selector(tick:)
                                           userInfo:nil repeats:YES];
    puts("[Identity] Waiting for compatible StopWatch firmware; no control events are generated.");
}

- (NSData *)payload
{
    NSError *error = nil;
    NSData *data = IdentityData(_hostID, LocalMacName(), &error);
    if (!data) fprintf(stderr, "[Identity] %s\n", error.localizedDescription.UTF8String);
    return data;
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
}

- (void)usbRemoved:(IOHIDDeviceRef)device
{
    NSValue *key = [NSValue valueWithPointer:device];
    if (!_usbStates[key]) return;
    [_usbStates removeObjectForKey:key];
    CFRelease(device);
    puts("[USB] StopWatch detached.");
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
}

- (void)tick:(NSTimer *)timer
{
    (void)timer;
    NSData *data = [self payload];
    for (NSValue *key in _usbStates.allKeys) [self sendUSB:(IOHIDDeviceRef)key.pointerValue payload:data];
    for (BLESession *session in _sessions.allValues) {
        if (((session.deadline && [session.deadline timeIntervalSinceNow] < 0) ||
             (session.quotaDeadline && [session.quotaDeadline timeIntervalSinceNow] < 0)) && !session.closing) {
            fprintf(stderr, "[BLE] Identity handshake timeout for %s.\n", session.peripheral.identifier.UUIDString.UTF8String);
            [self closeSession:session];
        } else if (session.identity && !session.pending && !session.closing) {
            [self sendBLE:session payload:data];
        }
    }
    [self findConnected];
    [self refreshQuotaIfReady];
}

- (void)findConnected
{
    if (_central.state != CBManagerStatePoweredOn) return;
    for (CBPeripheral *peripheral in [_central retrieveConnectedPeripheralsWithServices:@[ServiceUUID()]]) {
        [self connect:peripheral];
    }
    if (!_central.isScanning) [_central scanForPeripheralsWithServices:@[ServiceUUID()]
                                options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @YES}];
}

- (BOOL)hasUSBIdentity
{
    for (NSNumber *state in _usbStates.allValues) if (state.integerValue == 1) return YES;
    return NO;
}

- (void)refreshQuotaIfReady
{
    if (!_codexBin || _quotaReading || NSProcessInfo.processInfo.systemUptime < _nextQuotaRead) return;
    BOOL ready = [self hasUSBIdentity];
    for (BLESession *session in _sessions.allValues) if (session.reportedReady && !session.closing && session.quota) ready = YES;
    if (!ready) return;
    _quotaReading = YES;
    _nextQuotaRead = NSProcessInfo.processInfo.systemUptime + 60;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSError *error = nil;
        NSData *data = ReadCodexQuota(self->_codexBin, &error);
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_quotaReading = NO;
            if (!data) {
                fprintf(stderr, "[Quota] %s (next attempt in 60 seconds).\n", error.localizedDescription.UTF8String);
                return;
            }
            [self deliverQuota:data];
        });
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
    if (_sessions[peripheral.identifier] || [_retryAfter[peripheral.identifier] timeIntervalSinceNow] > 0) return;
    BLESession *session = [BLESession new];
    session.peripheral = peripheral;
    session.deadline = [NSDate dateWithTimeIntervalSinceNow:30];
    _sessions[peripheral.identifier] = session;
    peripheral.delegate = self;
    [_central connectPeripheral:peripheral options:nil];
}

- (void)closeSession:(BLESession *)session
{
    session.closing = YES;
    _retryAfter[session.peripheral.identifier] = [NSDate dateWithTimeIntervalSinceNow:15];
    [_central cancelPeripheralConnection:session.peripheral];
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
    if (central.state == CBManagerStatePoweredOn) {
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
}

- (void)centralManager:(CBCentralManager *)central didFailToConnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
    (void)central;
    fprintf(stderr, "[BLE] Connection failed: %s\n", error.localizedDescription.UTF8String ?: "unknown error");
    [_sessions removeObjectForKey:peripheral.identifier];
    _retryAfter[peripheral.identifier] = [NSDate dateWithTimeIntervalSinceNow:15];
}

- (void)centralManager:(CBCentralManager *)central didDisconnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
    (void)central;
    fprintf(stderr, "[BLE] Disconnected: %s\n", error.localizedDescription.UTF8String ?: "connection closed");
    [_sessions removeObjectForKey:peripheral.identifier];
    _retryAfter[peripheral.identifier] = [NSDate dateWithTimeIntervalSinceNow:15];
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error
{
    BLESession *session = _sessions[peripheral.identifier];
    if (!session || session.closing) return;
    if (error) { fprintf(stderr, "[BLE] Service discovery: %s\n", error.localizedDescription.UTF8String); [self closeSession:session]; return; }
    for (CBService *service in peripheral.services) {
        if ([service.UUID isEqual:ServiceUUID()]) {
            [peripheral discoverCharacteristics:@[IdentityUUID(), QuotaUUID()] forService:service];
            return;
        }
    }
    puts("[BLE] Identity service absent; waiting for compatible firmware.");
    [self closeSession:session];
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverCharacteristicsForService:(CBService *)service error:(NSError *)error
{
    BLESession *session = _sessions[peripheral.identifier];
    if (!session || session.closing) return;
    if (error) { fprintf(stderr, "[BLE] Characteristic discovery: %s\n", error.localizedDescription.UTF8String); [self closeSession:session]; return; }
    for (CBCharacteristic *characteristic in service.characteristics) {
        if ([characteristic.UUID isEqual:IdentityUUID()] && (characteristic.properties & CBCharacteristicPropertyWrite)) {
            session.identity = characteristic;
        }
        if ([characteristic.UUID isEqual:QuotaUUID()] && (characteristic.properties & CBCharacteristicPropertyWrite)) session.quota = characteristic;
    }
    if (session.identity) { [self sendBLE:session payload:[self payload]]; return; }
    puts("[BLE] Identity characteristic absent; firmware update or Bluetooth re-pair may be required.");
    [self closeSession:session];
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
    session.deadline = [NSDate dateWithTimeIntervalSinceNow:30];
    // Firmware requires encryption. CoreBluetooth negotiates pairing as needed.
    [session.peripheral writeValue:data forCharacteristic:session.identity type:CBCharacteristicWriteWithResponse];
}

- (void)peripheral:(CBPeripheral *)peripheral didWriteValueForCharacteristic:(CBCharacteristic *)characteristic error:(NSError *)error
{
    BLESession *session = _sessions[peripheral.identifier];
    if (!session) return;
    if ([characteristic.UUID isEqual:QuotaUUID()]) {
        session.quotaPending = NO;
        session.quotaDeadline = nil;
        if (error) fprintf(stderr, "[BLE] Quota write failed: %s\n", error.localizedDescription.UTF8String);
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
    if (!session.reportedReady) puts("[BLE] Identity accepted on encrypted characteristic; firmware decides the active host.");
    session.reportedReady = YES;
}
@end

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
    printf("{\"selfTest\":\"passed\",\"checks\":%lu,\"hardwareAccessed\":false}\n", (unsigned long)checks);
    return 0;
}

static void Usage(void)
{
    puts("Usage: codex-host-identity [--help | --self-test | --show-identity] [--codex-bin PATH | --no-quota]\n"
         "  No arguments: announce this Mac over BLE/USB and sync Codex quota.\n"
         "  --show-identity  Print/create the local identity without accessing hardware.\n"
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
        BOOL showIdentity = NO, quotaEnabled = YES;
        NSString *codexBin = nil;
        for (int index = 1; index < argc; ++index) {
            if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0) { Usage(); return 0; }
            if (strcmp(argv[index], "--self-test") == 0 && argc == 2) return SelfTest();
            if (strcmp(argv[index], "--show-identity") == 0) showIdentity = YES;
            else if (strcmp(argv[index], "--no-quota") == 0) quotaEnabled = NO;
            else if (strcmp(argv[index], "--codex-bin") == 0 && index + 1 < argc) {
                codexBin = [NSString stringWithUTF8String:argv[++index]];
                if (!codexBin.isAbsolutePath) { fprintf(stderr, "--codex-bin requires an absolute path.\n"); return 2; }
            } else { Usage(); return 2; }
        }
        NSURL *support = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
        NSURL *directory = [support URLByAppendingPathComponent:@"Friday/Codex Host Identity" isDirectory:YES];
        NSError *error = nil;
        NSString *hostID = LoadIdentity(directory, &error);
        if (!hostID) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
        if (showIdentity) {
            NSData *data = IdentityData(hostID, LocalMacName(), &error);
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
        IdentityBridge *bridge = [[IdentityBridge alloc] initWithHostID:hostID codexBin:quotaEnabled ? codexBin : nil];
        [bridge start];
        [NSRunLoop.mainRunLoop run];
        close(runFD);
    }
    return 0;
}
