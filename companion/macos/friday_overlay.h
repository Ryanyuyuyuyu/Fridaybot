/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#import <AppKit/AppKit.h>

typedef NS_ENUM(uint8_t, FridayTravelDirection) {
    FridayTravelDirectionLeft = 1,
    FridayTravelDirectionRight = 2,
    FridayTravelDirectionUp = 3,
    FridayTravelDirectionDown = 4,
};

@interface FridayOverlayController : NSObject
@property(nonatomic, copy) void (^onReturnStarted)(FridayTravelDirection direction, uint8_t sequence);
@property(nonatomic, copy) void (^onReturnCompleted)(FridayTravelDirection direction, uint8_t sequence);
@property(nonatomic, readonly, getter=isPresent) BOOL present;
- (void)presentFromDirection:(FridayTravelDirection)direction
                        seed:(uint16_t)seed
                    sequence:(uint8_t)sequence;
- (void)returnToDevice;
- (void)dismissImmediately;
@end
