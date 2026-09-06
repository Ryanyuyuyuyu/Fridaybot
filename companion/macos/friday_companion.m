/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#import "friday_overlay.h"
#include <signal.h>

static CBUUID *FridayServiceUUID(void)
{
    return [CBUUID UUIDWithString:@"46524944-4159-0001-8000-00805F9B34FB"];
}

static CBUUID *FridayContextUUID(void)
{
    return [CBUUID UUIDWithString:@"46524944-4159-0002-8000-00805F9B34FB"];
}

static CBUUID *FridayTransferUUID(void)
{
    return [CBUUID UUIDWithString:@"46524944-4159-0003-8000-00805F9B34FB"];
}

typedef NS_ENUM(uint8_t, FridayTravelCommand) {
    FridayTravelCommandDeviceOffer = 1,
    FridayTravelCommandHostAccept = 2,
    FridayTravelCommandDeviceHidden = 3,
    FridayTravelCommandHostReturnBegin = 4,
    FridayTravelCommandHostHidden = 5,
    FridayTravelCommandDeviceRecall = 6,
    FridayTravelCommandCancel = 7,
};

typedef struct {
    FridayTravelCommand command;
    FridayTravelDirection direction;
    uint8_t expression;
    uint8_t poseVariant;
    uint8_t sequence;
    uint16_t seed;
    uint16_t startDelayMs;
} FridayTravelPacket;

static BOOL DecodeTravelPacket(NSData *data, FridayTravelPacket *packet)
{
    if (data.length != 10 || packet == NULL) {
        return NO;
    }
    uint8_t bytes[10];
    [data getBytes:bytes length:sizeof(bytes)];
    if (bytes[0] != 2 || bytes[1] < FridayTravelCommandDeviceOffer ||
        bytes[1] > FridayTravelCommandCancel || bytes[2] < FridayTravelDirectionLeft ||
        bytes[2] > FridayTravelDirectionDown) {
        return NO;
    }
    packet->command = (FridayTravelCommand)bytes[1];
    packet->direction = (FridayTravelDirection)bytes[2];
    packet->expression = bytes[3];
    packet->poseVariant = bytes[4];
    packet->sequence = bytes[5];
    packet->seed = (uint16_t)bytes[6] | ((uint16_t)bytes[7] << 8U);
    packet->startDelayMs = (uint16_t)bytes[8] | ((uint16_t)bytes[9] << 8U);
    return YES;
}

static NSData *EncodeTravelPacket(FridayTravelCommand command, FridayTravelDirection direction, uint8_t sequence)
{
    uint8_t bytes[10] = { 2, command, direction, 0, 0, sequence, 0, 0, 0, 0 };
    return [NSData dataWithBytes:bytes length:sizeof(bytes)];
}

typedef NS_ENUM(uint8_t, FridayHostState) {
    FridayHostStateWorking = 1,
    FridayHostStateUncertain = 2,
    FridayHostStateAway = 3,
    FridayHostStateReturned = 4,
    FridayHostStateMeeting = 5,
};

typedef NS_ENUM(uint8_t, FridayMonitorDirection) {
    FridayMonitorDirectionCentre = 0,
    FridayMonitorDirectionLeft = 1,
    FridayMonitorDirectionRight = 2,
};

static NSString *StateName(FridayHostState state)
{
    switch (state) {
        case FridayHostStateWorking: return @"working";
        case FridayHostStateUncertain: return @"uncertain";
        case FridayHostStateAway: return @"away";
        case FridayHostStateReturned: return @"returned";
        case FridayHostStateMeeting: return @"meeting";
    }
    return @"unknown";
}

static NSString *DirectionName(FridayMonitorDirection direction)
{
    switch (direction) {
        case FridayMonitorDirectionCentre: return @"centre";
        case FridayMonitorDirectionLeft: return @"left";
        case FridayMonitorDirectionRight: return @"right";
    }
    return @"unknown";
}

@interface FridayConfiguration : NSObject
@property(nonatomic) FridayMonitorDirection monitorDirection;
@property(nonatomic) BOOL dryRun;
@property(nonatomic) BOOL hasForcedState;
@property(nonatomic) FridayHostState forcedState;
@property(nonatomic) BOOL overlayEnabled;
+ (instancetype)parse;
@end

static void PrintUsageAndExit(NSString *error, int status)
{
    if (error != nil) {
        fprintf(stderr, "Error: %s\n", error.UTF8String);
    }
    puts("Usage: friday-companion [--monitor-side left|centre|right] [--force-state STATE] [--dry-run] [--no-overlay]\n"
         "\n"
         "  --monitor-side  Position of the monitor from Friday's point of view.\n"
         "  --force-state   Test working, uncertain, away, returned, or meeting.\n"
         "  --dry-run       Detect and print context without using Bluetooth.\n"
         "  --no-overlay    Disable the cross-device Friday window.");
    exit(status);
}

@implementation FridayConfiguration
+ (instancetype)parse
{
    FridayConfiguration *configuration = [[FridayConfiguration alloc] init];
    configuration.monitorDirection = FridayMonitorDirectionCentre;
    configuration.overlayEnabled = YES;
    NSArray<NSString *> *arguments = NSProcessInfo.processInfo.arguments;
    for (NSUInteger index = 1; index < arguments.count; ++index) {
        NSString *argument = arguments[index];
        if ([argument isEqualToString:@"--monitor-side"]) {
            if (++index >= arguments.count) {
                PrintUsageAndExit(@"Missing value after --monitor-side", 2);
            }
            NSString *value = arguments[index].lowercaseString;
            if ([value isEqualToString:@"left"]) {
                configuration.monitorDirection = FridayMonitorDirectionLeft;
            } else if ([value isEqualToString:@"right"]) {
                configuration.monitorDirection = FridayMonitorDirectionRight;
            } else if ([value isEqualToString:@"centre"] || [value isEqualToString:@"center"]) {
                configuration.monitorDirection = FridayMonitorDirectionCentre;
            } else {
                PrintUsageAndExit(@"Monitor side must be left, centre, or right", 2);
            }
        } else if ([argument isEqualToString:@"--dry-run"]) {
            configuration.dryRun = YES;
        } else if ([argument isEqualToString:@"--no-overlay"]) {
            configuration.overlayEnabled = NO;
        } else if ([argument isEqualToString:@"--force-state"]) {
            if (++index >= arguments.count) {
                PrintUsageAndExit(@"Missing value after --force-state", 2);
            }
            NSString *value = arguments[index].lowercaseString;
            NSDictionary<NSString *, NSNumber *> *states = @{
                @"working" : @(FridayHostStateWorking),
                @"uncertain" : @(FridayHostStateUncertain),
                @"away" : @(FridayHostStateAway),
                @"returned" : @(FridayHostStateReturned),
                @"meeting" : @(FridayHostStateMeeting),
            };
            NSNumber *state = states[value];
            if (state == nil) {
                PrintUsageAndExit(@"State must be working, uncertain, away, returned, or meeting", 2);
            }
            configuration.hasForcedState = YES;
            configuration.forcedState = (FridayHostState)state.unsignedCharValue;
        } else if ([argument isEqualToString:@"--help"] || [argument isEqualToString:@"-h"]) {
            PrintUsageAndExit(nil, 0);
        } else {
            PrintUsageAndExit([@"Unknown argument: " stringByAppendingString:argument], 2);
        }
    }
    return configuration;
}
@end

typedef struct {
    FridayHostState state;
    NSTimeInterval idleSeconds;
    BOOL locked;
} FridayPresenceSample;

@interface FridayPresenceDetector : NSObject
- (FridayPresenceSample)sample;
@end

@implementation FridayPresenceDetector {
    FridayHostState _previousBaseState;
    BOOL _hasPreviousState;
    NSDate *_returnedUntil;
}

- (instancetype)init
{
    self = [super init];
    if (self != nil) {
        _returnedUntil = NSDate.distantPast;
    }
    return self;
}

- (FridayPresenceSample)sample
{
    NSTimeInterval idle = CGEventSourceSecondsSinceLastEventType(
        kCGEventSourceStateCombinedSessionState, kCGAnyInputEventType);

    BOOL locked = NO;
    CFDictionaryRef session = CGSessionCopyCurrentDictionary();
    if (session != NULL) {
        CFBooleanRef value = CFDictionaryGetValue(session, CFSTR("CGSSessionScreenIsLocked"));
        locked = value != NULL && CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value);
        CFRelease(session);
    }

    FridayHostState baseState;
    if (locked || idle >= 5.0 * 60.0) {
        baseState = FridayHostStateAway;
    } else if (idle < 15.0) {
        baseState = FridayHostStateWorking;
    } else {
        baseState = FridayHostStateUncertain;
    }

    if (_hasPreviousState && _previousBaseState == FridayHostStateAway && baseState == FridayHostStateWorking) {
        _returnedUntil = [NSDate dateWithTimeIntervalSinceNow:3.0];
    }
    _previousBaseState = baseState;
    _hasPreviousState = YES;

    FridayPresenceSample sample = {
        .state = [[NSDate date] compare:_returnedUntil] == NSOrderedAscending ? FridayHostStateReturned : baseState,
        .idleSeconds = idle,
        .locked = locked,
    };
    return sample;
}
@end

@interface FridayBluetooth : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property(nonatomic, copy) void (^onTravelPacket)(FridayTravelPacket packet);
@property(nonatomic, copy) void (^onDisconnected)(void);
- (void)updateState:(FridayHostState)state direction:(FridayMonitorDirection)direction;
- (void)sendTravelCommand:(FridayTravelCommand)command
                 direction:(FridayTravelDirection)direction
                  sequence:(uint8_t)sequence;
- (void)stop;
@end

@implementation FridayBluetooth {
    CBCentralManager *_central;
    CBPeripheral *_peripheral;
    CBCharacteristic *_contextCharacteristic;
    CBCharacteristic *_transferCharacteristic;
    FridayHostState _pendingState;
    FridayMonitorDirection _pendingDirection;
    BOOL _hasPendingState;
    FridayHostState _lastSentState;
    FridayMonitorDirection _lastSentDirection;
    BOOL _hasSentState;
    NSDate *_lastSentAt;
    uint8_t _sequence;
    NSUInteger _connectionGeneration;
}

- (instancetype)init
{
    self = [super init];
    if (self != nil) {
        _lastSentAt = NSDate.distantPast;
        _central = [[CBCentralManager alloc] initWithDelegate:self queue:dispatch_get_main_queue()];
    }
    return self;
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
    switch (central.state) {
        case CBManagerStatePoweredOn:
            [self findOrScan];
            break;
        case CBManagerStatePoweredOff:
            puts("[BLE] Bluetooth is off; Friday remains in local mode");
            break;
        case CBManagerStateUnauthorized:
            puts("[BLE] Bluetooth permission denied; allow Friday Companion in System Settings");
            break;
        default:
            break;
    }
}

- (void)findOrScan
{
    NSArray<CBPeripheral *> *connected = [_central retrieveConnectedPeripheralsWithServices:@[ FridayServiceUUID() ]];
    if (connected.count == 0) {
        [self scan];
        return;
    }

    _peripheral = connected.firstObject;
    puts("[BLE] Reusing an existing Friday connection…");
    [_central connectPeripheral:_peripheral options:nil];
}

- (void)scan
{
    if (_peripheral != nil || _central.state != CBManagerStatePoweredOn) {
        return;
    }
    puts("[BLE] Looking for Friday…");
    [_central scanForPeripheralsWithServices:@[ FridayServiceUUID() ]
                                    options:@{ CBCentralManagerScanOptionAllowDuplicatesKey : @NO }];
}

- (void)centralManager:(CBCentralManager *)central
 didDiscoverPeripheral:(CBPeripheral *)peripheral
     advertisementData:(NSDictionary<NSString *, id> *)advertisementData
                  RSSI:(NSNumber *)RSSI
{
    (void)advertisementData;
    _peripheral = peripheral;
    [central stopScan];
    printf("[BLE] Found Friday (RSSI %d); connecting…\n", RSSI.intValue);
    NSUInteger generation = ++_connectionGeneration;
    [central connectPeripheral:peripheral options:nil];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 45 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (generation == self->_connectionGeneration && self->_contextCharacteristic == nil) {
            puts("[BLE] Connection timed out; retrying…");
            [self->_central cancelPeripheralConnection:peripheral];
            [self resetAndRetry];
        }
    });
}

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)peripheral
{
    (void)central;
    puts("[BLE] Connected");
    peripheral.delegate = self;
    // Discover the complete database once. This also bypasses a stale,
    // UUID-filtered CoreBluetooth cache left by earlier firmware identities.
    [peripheral discoverServices:nil];
    NSUInteger generation = _connectionGeneration;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (generation == self->_connectionGeneration && self->_contextCharacteristic == nil) {
            puts("[BLE] GATT handshake timed out; reconnecting…");
            [self->_central cancelPeripheralConnection:peripheral];
            [self resetAndRetry];
        }
    });
}

- (void)centralManager:(CBCentralManager *)central
didFailToConnectPeripheral:(CBPeripheral *)peripheral
                 error:(NSError *)error
{
    (void)central;
    (void)peripheral;
    printf("[BLE] Connection failed: %s\n", error.localizedDescription.UTF8String ?: "unknown error");
    [self resetAndRetry];
}

- (void)centralManager:(CBCentralManager *)central
didDisconnectPeripheral:(CBPeripheral *)peripheral
                  error:(NSError *)error
{
    (void)central;
    (void)peripheral;
    (void)error;
    puts("[BLE] Disconnected; Friday has returned to local mode");
    if (self.onDisconnected != nil) {
        self.onDisconnected();
    }
    [self resetAndRetry];
}

- (void)resetAndRetry
{
    ++_connectionGeneration;
    _peripheral = nil;
    _contextCharacteristic = nil;
    _transferCharacteristic = nil;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        [self findOrScan];
    });
}

- (void)stop
{
    ++_connectionGeneration;
    [_central stopScan];
    if (_peripheral != nil) {
        [_central cancelPeripheralConnection:_peripheral];
    }
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error
{
    if (error != nil) {
        printf("[BLE] Service discovery failed: %s\n", error.localizedDescription.UTF8String);
        [_central cancelPeripheralConnection:peripheral];
        return;
    }
    puts("[BLE] Services discovered");
    for (CBService *service in peripheral.services) {
        printf("[BLE]   service %s\n", service.UUID.UUIDString.UTF8String);
        if ([service.UUID isEqual:FridayServiceUUID()]) {
            [peripheral discoverCharacteristics:nil forService:service];
            return;
        }
    }
    puts("[BLE] Friday context service is missing; reconnecting…");
    [_central cancelPeripheralConnection:peripheral];
}

- (void)peripheral:(CBPeripheral *)peripheral
didDiscoverCharacteristicsForService:(CBService *)service
             error:(NSError *)error
{
    if (error != nil) {
        printf("[BLE] Characteristic discovery failed: %s\n", error.localizedDescription.UTF8String);
        [_central cancelPeripheralConnection:peripheral];
        return;
    }
    for (CBCharacteristic *characteristic in service.characteristics) {
        printf("[BLE]   characteristic %s\n", characteristic.UUID.UUIDString.UTF8String);
        if ([characteristic.UUID isEqual:FridayContextUUID()]) {
            _contextCharacteristic = characteristic;
        } else if ([characteristic.UUID isEqual:FridayTransferUUID()]) {
            _transferCharacteristic = characteristic;
        }
    }
    if (_contextCharacteristic == nil || _transferCharacteristic == nil) {
        puts("[BLE] Friday context or travel characteristic is missing");
        [_central cancelPeripheralConnection:peripheral];
        return;
    }
    ++_connectionGeneration;
    puts("[BLE] Context channel ready");
    [peripheral setNotifyValue:YES forCharacteristic:_transferCharacteristic];
    [self flushPending];
}

- (void)peripheral:(CBPeripheral *)peripheral
didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)characteristic
             error:(NSError *)error
{
    (void)peripheral;
    if (![characteristic.UUID isEqual:FridayTransferUUID()]) {
        return;
    }
    if (error != nil) {
        printf("[BLE] Travel notification setup failed: %s\n", error.localizedDescription.UTF8String);
        return;
    }
    printf("[BLE] Travel channel %s\n", characteristic.isNotifying ? "ready" : "disabled");
}

- (void)peripheral:(CBPeripheral *)peripheral
didUpdateValueForCharacteristic:(CBCharacteristic *)characteristic
             error:(NSError *)error
{
    (void)peripheral;
    if (![characteristic.UUID isEqual:FridayTransferUUID()]) {
        return;
    }
    if (error != nil) {
        printf("[BLE] Travel notification failed: %s\n", error.localizedDescription.UTF8String);
        return;
    }
    FridayTravelPacket packet;
    if (!DecodeTravelPacket(characteristic.value, &packet)) {
        puts("[BLE] Ignoring malformed travel packet");
        return;
    }
    printf("[BLE] Received travel command %u, direction=%u, sequence=%u\n",
           (unsigned)packet.command, (unsigned)packet.direction, (unsigned)packet.sequence);
    if (self.onTravelPacket != nil) {
        self.onTravelPacket(packet);
    }
}

- (void)updateState:(FridayHostState)state direction:(FridayMonitorDirection)direction
{
    _pendingState = state;
    _pendingDirection = direction;
    _hasPendingState = YES;
    BOOL changed = !_hasSentState || state != _lastSentState || direction != _lastSentDirection;
    BOOL heartbeatDue = -[_lastSentAt timeIntervalSinceNow] >= 15.0;
    if (changed || heartbeatDue) {
        [self flushPending];
    }
}

- (void)flushPending
{
    if (!_hasPendingState || _peripheral == nil || _contextCharacteristic == nil) {
        return;
    }

    _sequence += 1;
    uint8_t bytes[4] = { 1, _pendingState, _pendingDirection, _sequence };
    NSData *packet = [NSData dataWithBytes:bytes length:sizeof(bytes)];
    // Four bytes every 15 seconds is tiny. Prefer an acknowledged write so a
    // relaxed BLE connection cannot silently drop a heartbeat.
    CBCharacteristicWriteType type = (_contextCharacteristic.properties & CBCharacteristicPropertyWrite)
        ? CBCharacteristicWriteWithResponse : CBCharacteristicWriteWithoutResponse;
    [_peripheral writeValue:packet forCharacteristic:_contextCharacteristic type:type];
    _lastSentState = _pendingState;
    _lastSentDirection = _pendingDirection;
    _hasSentState = YES;
    _lastSentAt = [NSDate date];
    printf("[BLE] Sent %s, monitor=%s, sequence=%u\n",
           StateName(_pendingState).UTF8String, DirectionName(_pendingDirection).UTF8String, _sequence);
}

- (void)sendTravelCommand:(FridayTravelCommand)command
                 direction:(FridayTravelDirection)direction
                  sequence:(uint8_t)sequence
{
    if (_peripheral == nil || _transferCharacteristic == nil) {
        puts("[BLE] Cannot send travel command; channel is unavailable");
        return;
    }
    NSData *packet = EncodeTravelPacket(command, direction, sequence);
    CBCharacteristicWriteType type = (_transferCharacteristic.properties & CBCharacteristicPropertyWrite)
        ? CBCharacteristicWriteWithResponse : CBCharacteristicWriteWithoutResponse;
    [_peripheral writeValue:packet forCharacteristic:_transferCharacteristic type:type];
    printf("[BLE] Sent travel command %u, direction=%u, sequence=%u\n",
           (unsigned)command, (unsigned)direction, (unsigned)sequence);
}

- (void)peripheral:(CBPeripheral *)peripheral
didWriteValueForCharacteristic:(CBCharacteristic *)characteristic
             error:(NSError *)error
{
    (void)peripheral;
    if ([characteristic.UUID isEqual:FridayContextUUID()] && error != nil) {
        printf("[BLE] Context write failed: %s\n", error.localizedDescription.UTF8String);
    } else if ([characteristic.UUID isEqual:FridayTransferUUID()] && error != nil) {
        printf("[BLE] Travel write failed: %s\n", error.localizedDescription.UTF8String);
    }
}
@end

@interface FridayController : NSObject
- (instancetype)initWithConfiguration:(FridayConfiguration *)configuration;
- (void)handleTravelPacket:(FridayTravelPacket)packet;
- (void)start;
- (void)stop;
@end

@implementation FridayController {
    FridayConfiguration *_configuration;
    FridayPresenceDetector *_detector;
    FridayBluetooth *_bluetooth;
    FridayOverlayController *_overlay;
    BOOL _hasPrintedState;
    FridayHostState _lastPrintedState;
    BOOL _hasPendingTravel;
    FridayTravelPacket _pendingTravel;
    BOOL _hasActiveTravel;
    uint8_t _activeTravelSequence;
}

- (instancetype)initWithConfiguration:(FridayConfiguration *)configuration
{
    self = [super init];
    if (self != nil) {
        _configuration = configuration;
        _detector = [[FridayPresenceDetector alloc] init];
        if (!configuration.dryRun) {
            _bluetooth = [[FridayBluetooth alloc] init];
            if (configuration.overlayEnabled) {
                _overlay = [[FridayOverlayController alloc] init];
            }
            __weak FridayController *weakSelf = self;
            _bluetooth.onTravelPacket = ^(FridayTravelPacket packet) {
                [weakSelf handleTravelPacket:packet];
            };
            _bluetooth.onDisconnected = ^{
                FridayController *strongSelf = weakSelf;
                [strongSelf->_overlay dismissImmediately];
                strongSelf->_hasPendingTravel = NO;
                strongSelf->_hasActiveTravel = NO;
            };
            _overlay.onReturnStarted = ^(FridayTravelDirection direction, uint8_t sequence) {
                FridayController *strongSelf = weakSelf;
                [strongSelf->_bluetooth sendTravelCommand:FridayTravelCommandHostReturnBegin
                                                 direction:direction
                                                  sequence:sequence];
            };
            _overlay.onReturnCompleted = ^(FridayTravelDirection direction, uint8_t sequence) {
                FridayController *strongSelf = weakSelf;
                // The panel has already been ordered out. HostHidden is the
                // barrier that permits the StopWatch's first visible frame.
                [strongSelf->_bluetooth sendTravelCommand:FridayTravelCommandHostHidden
                                                 direction:direction
                                                  sequence:sequence];
                if (strongSelf->_hasActiveTravel && strongSelf->_activeTravelSequence == sequence) {
                    strongSelf->_hasActiveTravel = NO;
                }
            };
        }
    }
    return self;
}

- (void)handleTravelPacket:(FridayTravelPacket)packet
{
    if (packet.command == FridayTravelCommandDeviceOffer) {
        if (_overlay == nil) {
            [_bluetooth sendTravelCommand:FridayTravelCommandCancel
                                direction:packet.direction
                                 sequence:packet.sequence];
            return;
        }
        if (_hasActiveTravel || _overlay.isPresent) {
            if (_hasActiveTravel && packet.sequence == _activeTravelSequence) {
                [_bluetooth sendTravelCommand:FridayTravelCommandHostAccept
                                    direction:packet.direction
                                     sequence:packet.sequence];
            } else {
                [_bluetooth sendTravelCommand:FridayTravelCommandCancel
                                    direction:packet.direction
                                     sequence:packet.sequence];
            }
            return;
        }
        // Accept only reserves the destination. Nothing is drawn until the
        // StopWatch later publishes DeviceHidden for the same sequence.
        _pendingTravel = packet;
        _hasPendingTravel = YES;
        [_bluetooth sendTravelCommand:FridayTravelCommandHostAccept
                            direction:packet.direction
                             sequence:packet.sequence];
    } else if (packet.command == FridayTravelCommandDeviceHidden) {
        if (_hasActiveTravel && packet.sequence == _activeTravelSequence) {
            return;  // Idempotent retry: never create a second panel.
        }
        if (!_hasPendingTravel || packet.sequence != _pendingTravel.sequence || _overlay.isPresent) {
            [_bluetooth sendTravelCommand:FridayTravelCommandCancel
                                direction:packet.direction
                                 sequence:packet.sequence];
            return;
        }
        _hasPendingTravel = NO;
        _hasActiveTravel = YES;
        _activeTravelSequence = packet.sequence;
        [_overlay presentFromDirection:packet.direction
                                  seed:packet.seed
                              sequence:packet.sequence];
    } else if (packet.command == FridayTravelCommandDeviceRecall) {
        if (_hasActiveTravel && packet.sequence == _activeTravelSequence && _overlay.isPresent) {
            [_overlay returnToDevice];
        } else {
            // No visible panel means both barriers can be acknowledged
            // immediately without risking overlapping faces.
            [_bluetooth sendTravelCommand:FridayTravelCommandHostReturnBegin
                                direction:packet.direction
                                 sequence:packet.sequence];
            [_bluetooth sendTravelCommand:FridayTravelCommandHostHidden
                                direction:packet.direction
                                 sequence:packet.sequence];
            _hasActiveTravel = NO;
        }
    } else if (packet.command == FridayTravelCommandCancel) {
        [_overlay dismissImmediately];
        _hasPendingTravel = NO;
        _hasActiveTravel = NO;
    }
}

- (void)start
{
    printf("Friday Companion — monitor is %s from Friday's point of view\n",
           DirectionName(_configuration.monitorDirection).UTF8String);
    if (_configuration.dryRun) {
        puts("Dry-run mode: Bluetooth is disabled");
    }
    if (_configuration.hasForcedState) {
        printf("Test override: forcing %s\n", StateName(_configuration.forcedState).UTF8String);
    }
    [self tick:nil];
    [NSTimer scheduledTimerWithTimeInterval:1.0 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
}

- (void)tick:(NSTimer *)timer
{
    (void)timer;
    FridayPresenceSample sample = [_detector sample];
    if (_configuration.hasForcedState) {
        sample.state = _configuration.forcedState;
    }
    if (!_hasPrintedState || sample.state != _lastPrintedState) {
        printf("[Context] %s, idle=%lds, locked=%s\n", StateName(sample.state).UTF8String,
               (long)sample.idleSeconds, sample.locked ? "true" : "false");
        _lastPrintedState = sample.state;
        _hasPrintedState = YES;
    }
    [_bluetooth updateState:sample.state direction:_configuration.monitorDirection];
}

- (void)stop
{
    puts("\nFriday Companion is stopping; releasing the BLE connection…");
    [_bluetooth stop];
    [_overlay dismissImmediately];
}
@end

int main(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    setvbuf(stdout, NULL, _IOLBF, 0);
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        FridayConfiguration *configuration = [FridayConfiguration parse];
        FridayController *controller = [[FridayController alloc] initWithConfiguration:configuration];
        [controller start];

        signal(SIGINT, SIG_IGN);
        signal(SIGTERM, SIG_IGN);
        dispatch_source_t interruptSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0,
                                                                    dispatch_get_main_queue());
        dispatch_source_t terminateSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0,
                                                                    dispatch_get_main_queue());
        void (^stopHandler)(void) = ^{
            [controller stop];
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
                exit(0);
            });
        };
        dispatch_source_set_event_handler(interruptSource, stopHandler);
        dispatch_source_set_event_handler(terminateSource, stopHandler);
        dispatch_resume(interruptSource);
        dispatch_resume(terminateSource);
        // NSRunLoop alone services timers and CoreBluetooth but does not ask
        // NSApplication to dispatch NSEvents to the borderless overlay. Run
        // the full AppKit loop so Friday can actually receive click/drag input
        // while remaining an accessory app without a Dock icon.
        [NSApp run];
    }
    return 0;
}
