/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#import "friday_overlay.h"
#include <math.h>
#include <stdio.h>

static const CGFloat FridayOrbSize = 168.0;
static const CGFloat FridayEdgeTuck = 18.0;

typedef NS_ENUM(uint8_t, FridayOverlayPhase) {
    FridayOverlayPhaseHidden,
    FridayOverlayPhasePeeking,
    FridayOverlayPhaseWatching,
    FridayOverlayPhaseRetreating,
    FridayOverlayPhaseEntering,
    FridayOverlayPhaseIdle,
    FridayOverlayPhaseDragging,
    FridayOverlayPhaseSettling,
    FridayOverlayPhaseExploring,
    FridayOverlayPhaseNesting,
    FridayOverlayPhaseEdgeResting,
    FridayOverlayPhaseWaking,
    FridayOverlayPhaseReturning,
};

static CGFloat Clamp(CGFloat value, CGFloat minimum, CGFloat maximum)
{
    return fmax(minimum, fmin(maximum, value));
}

static CGFloat SmoothStep(CGFloat value)
{
    value = Clamp(value, 0.0, 1.0);
    return value * value * (3.0 - 2.0 * value);
}

static NSPoint InterpolatePoint(NSPoint from, NSPoint to, CGFloat progress)
{
    return NSMakePoint(from.x + (to.x - from.x) * progress,
                       from.y + (to.y - from.y) * progress);
}

static CGFloat RandomUnit(uint32_t value)
{
    return (CGFloat)(value & 0x00FFFFFFU) / (CGFloat)0x01000000U;
}

@interface FridayOverlayView : NSView
@property(nonatomic) CGFloat animationTime;
@property(nonatomic) NSTimeInterval reactionStartedAt;
@property(nonatomic) uint16_t seed;
@property(nonatomic) BOOL beingDragged;
@property(nonatomic) CGFloat dragEnergy;
@property(nonatomic) CGFloat autonomousGazeX;
@property(nonatomic) CGFloat autonomousGazeY;
@property(nonatomic) CGFloat boundaryX;
@property(nonatomic) CGFloat boundaryY;
@property(nonatomic) CGFloat boundaryStrength;
@property(nonatomic) BOOL restingAtBoundary;
@property(nonatomic, copy) void (^onPointerDown)(NSPoint screenPoint);
@property(nonatomic, copy) void (^onPointerDragged)(NSPoint screenPoint);
@property(nonatomic, copy) void (^onPointerUp)(NSPoint screenPoint, BOOL dragged);
- (void)playClickReaction;
@end

@implementation FridayOverlayView {
}

- (BOOL)isFlipped
{
    return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
    (void)event;
    return YES;
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (NSView *)hitTest:(NSPoint)point
{
    // NSPanel is deliberately transparent and non-activating. Explicitly own
    // the circular window's hit region so AppKit never treats its black or
    // transparent pixels as click-through content.
    return NSPointInRect(point, self.bounds) ? self : nil;
}

- (void)resetCursorRects
{
    [super resetCursorRects];
    [self addCursorRect:self.bounds cursor:NSCursor.openHandCursor];
}

- (void)mouseDown:(NSEvent *)event
{
    NSPoint pointerDownPoint = NSEvent.mouseLocation;
    BOOL didDrag = NO;
    if (self.onPointerDown != nil) {
        self.onPointerDown(pointerDownPoint);
    }

    // A borderless non-activating panel does not reliably receive a normal
    // mouseDragged chain on every macOS/Spaces arrangement. Own the tracking
    // session explicitly so the Friday window follows the pointer until the
    // corresponding mouse-up, even while crossing display boundaries.
    const NSEventMask mask = NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp;
    while (true) {
        NSEvent *next = [NSApp nextEventMatchingMask:mask
                                          untilDate:NSDate.distantFuture
                                             inMode:NSEventTrackingRunLoopMode
                                            dequeue:YES];
        if (next == nil) {
            continue;
        }
        NSPoint point = NSEvent.mouseLocation;
        if (next.type == NSEventTypeLeftMouseDragged) {
            if (hypot(point.x - pointerDownPoint.x, point.y - pointerDownPoint.y) >= 4.0) {
                didDrag = YES;
            }
            if (didDrag && self.onPointerDragged != nil) {
                [NSCursor.closedHandCursor set];
                self.onPointerDragged(point);
            }
        } else if (next.type == NSEventTypeLeftMouseUp) {
            if (self.onPointerUp != nil) {
                self.onPointerUp(point, didDrag);
            }
            [NSCursor.openHandCursor set];
            break;
        }
    }
    (void)event;
}

- (void)playClickReaction
{
    self.reactionStartedAt = NSDate.timeIntervalSinceReferenceDate;
}

- (void)drawRect:(NSRect)dirtyRect
{
    (void)dirtyRect;
    [[NSColor clearColor] setFill];
    NSRectFill(self.bounds);

    CGFloat time = self.animationTime;
    CGFloat boundaryBreath = 0.82 + sin(time * 2.0 * M_PI / 3.8) * 0.18;
    CGFloat contact = Clamp(self.boundaryStrength * boundaryBreath, 0.0, 1.0);
    CGFloat horizontalContact = fabs(self.boundaryX) * contact;
    CGFloat verticalContact = fabs(self.boundaryY) * contact;

    // The black body deforms before the window is clipped by the physical
    // display edge. This makes the bezel read as a surface Friday is pressing
    // against instead of an arbitrary crop of a floating circle.
    NSRect orbRect = NSInsetRect(self.bounds, 6.0, 6.0);
    CGFloat originalWidth = NSWidth(orbRect);
    CGFloat originalHeight = NSHeight(orbRect);
    orbRect.size.width *= 1.0 - horizontalContact * 0.10 + verticalContact * 0.035;
    orbRect.size.height *= 1.0 - verticalContact * 0.10 + horizontalContact * 0.035;
    orbRect.origin.x += (originalWidth - NSWidth(orbRect)) * 0.5 + self.boundaryX * contact * 4.0;
    orbRect.origin.y += (originalHeight - NSHeight(orbRect)) * 0.5 + self.boundaryY * contact * 4.0;
    NSBezierPath *orb = [NSBezierPath bezierPathWithOvalInRect:orbRect];
    [[NSColor colorWithWhite:0.005 alpha:0.98] setFill];
    [orb fill];
    [[NSColor colorWithWhite:0.18 alpha:0.75] setStroke];
    orb.lineWidth = 1.0;
    [orb stroke];

    NSPoint mouse = NSEvent.mouseLocation;
    NSPoint centreOnScreen = NSMakePoint(NSMidX(self.window.frame), NSMidY(self.window.frame));
    CGFloat gazeX = Clamp((mouse.x - centreOnScreen.x) / 38.0, -1.0, 1.0);
    CGFloat gazeY = Clamp((mouse.y - centreOnScreen.y) / 38.0, -1.0, 1.0);
    if (fabs(self.autonomousGazeX) > 0.01 || fabs(self.autonomousGazeY) > 0.01) {
        gazeX = Clamp(gazeX * 0.30 + self.autonomousGazeX, -1.0, 1.0);
        gazeY = Clamp(gazeY * 0.30 + self.autonomousGazeY, -1.0, 1.0);
    } else if (self.restingAtBoundary) {
        gazeX *= 0.28;
        gazeY *= 0.28;
    }
    CGFloat bob = self.beingDragged || self.restingAtBoundary
                      ? 0.0
                      : sin(time * 2.0 * M_PI / 2.8) * 2.2;

    // A deterministic blink phase means a transferred Friday continues
    // moving locally without frame streaming or network jitter.
    CGFloat blinkCycle = 4.2 + (self.seed % 170) / 100.0;
    CGFloat blinkPhase = fmod(time + (self.seed % 100) / 37.0, blinkCycle);
    CGFloat blink = 1.0;
    if (blinkPhase < 0.28 && !self.beingDragged) {
        CGFloat phase = blinkPhase / 0.28;
        blink = phase < 0.38 ? 1.0 - SmoothStep(phase / 0.38) * 0.93
                             : 0.07 + SmoothStep((phase - 0.38) / 0.62) * 0.93;
    }

    CGFloat reaction = 0.0;
    NSTimeInterval reactionAge = NSDate.timeIntervalSinceReferenceDate - self.reactionStartedAt;
    if (self.reactionStartedAt > 0.0 && reactionAge < 0.82) {
        CGFloat phase = Clamp(reactionAge / 0.82, 0.0, 1.0);
        reaction = sin(phase * M_PI) * (1.0 - phase * 0.22);
        bob -= reaction * 7.0;
    }

    CGFloat strength = SmoothStep(fabs(gazeX));
    CGFloat spacing = 58.0 * (1.0 - strength * 0.10) * (1.0 - reaction * 0.10);
    CGFloat leftWidth = 25.0;
    CGFloat rightWidth = 25.0;
    CGFloat leftHeight = 57.0 * (1.0 + reaction * 0.16);
    CGFloat rightHeight = 57.0 * (1.0 + reaction * 0.16);
    if (gazeX > 0.0) {
        leftWidth *= 1.0 - strength * 0.25;
        leftHeight *= 1.0 - strength * 0.07;
        rightWidth *= 1.0 + strength * 0.10;
        rightHeight *= 1.0 + strength * 0.03;
    } else {
        leftWidth *= 1.0 + strength * 0.10;
        leftHeight *= 1.0 + strength * 0.03;
        rightWidth *= 1.0 - strength * 0.25;
        rightHeight *= 1.0 - strength * 0.07;
    }

    CGFloat dragSquash = Clamp(self.dragEnergy / 34.0, 0.0, 1.0) * 0.12;
    leftWidth *= 1.0 + dragSquash;
    rightWidth *= 1.0 + dragSquash;
    leftHeight *= 1.0 - dragSquash * 0.75;
    rightHeight *= 1.0 - dragSquash * 0.75;

    if (horizontalContact > 0.0) {
        spacing *= 1.0 - horizontalContact * 0.10;
        if (self.boundaryX < 0.0) {
            leftWidth *= 1.0 - horizontalContact * 0.24;
            leftHeight *= 1.0 + horizontalContact * 0.06;
            rightWidth *= 1.0 - horizontalContact * 0.04;
        } else {
            rightWidth *= 1.0 - horizontalContact * 0.24;
            rightHeight *= 1.0 + horizontalContact * 0.06;
            leftWidth *= 1.0 - horizontalContact * 0.04;
        }
    }
    if (verticalContact > 0.0) {
        leftWidth *= 1.0 + verticalContact * 0.08;
        rightWidth *= 1.0 + verticalContact * 0.08;
        leftHeight *= 1.0 - verticalContact * 0.18;
        rightHeight *= 1.0 - verticalContact * 0.18;
    }
    if (self.restingAtBoundary) {
        CGFloat sleepyBreath = 0.76 + sin(time * 2.0 * M_PI / 4.6) * 0.035;
        leftHeight *= sleepyBreath;
        rightHeight *= sleepyBreath * 0.96;
    }
    leftHeight = fmax(5.0, leftHeight * blink);
    rightHeight = fmax(5.0, rightHeight * blink);

    CGFloat centreX = NSMidX(self.bounds) + gazeX * 9.0 + self.boundaryX * contact * 5.0;
    CGFloat centreY = NSMidY(self.bounds) - gazeY * 6.0 - 7.0 + bob + self.boundaryY * contact * 5.0;
    NSRect leftRect = NSMakeRect(centreX - spacing * 0.5 - leftWidth * 0.5,
                                 centreY - leftHeight * 0.5 + strength * (gazeX > 0.0 ? 2.0 : -1.0),
                                 leftWidth, leftHeight);
    NSRect rightRect = NSMakeRect(centreX + spacing * 0.5 - rightWidth * 0.5,
                                  centreY - rightHeight * 0.5 + strength * (gazeX < 0.0 ? 2.0 : -1.0),
                                  rightWidth, rightHeight);

    [[NSColor colorWithCalibratedWhite:0.93 alpha:1.0] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:leftRect xRadius:leftWidth * 0.5 yRadius:leftWidth * 0.5] fill];
    [[NSBezierPath bezierPathWithRoundedRect:rightRect xRadius:rightWidth * 0.5 yRadius:rightWidth * 0.5] fill];
}

@end

@interface FridayOverlayController ()
- (void)scheduleNextExploreFrom:(NSTimeInterval)now;
- (void)beginExplorationAt:(NSTimeInterval)now;
- (void)beginEdgeRestAt:(NSTimeInterval)now;
- (void)wakeFromEdgeAt:(NSTimeInterval)now;
- (void)clearBoundaryPose;
- (uint32_t)nextAutonomyRandom;
@end

@implementation FridayOverlayController {
    NSPanel *_panel;
    FridayOverlayView *_view;
    NSTimer *_timer;
    FridayOverlayPhase _phase;
    FridayTravelDirection _direction;
    uint8_t _sequence;
    NSTimeInterval _phaseStartedAt;
    NSTimeInterval _visibleStartedAt;
    NSPoint _hiddenOrigin;
    NSPoint _peekOrigin;
    NSPoint _shyOrigin;
    NSPoint _restOrigin;
    NSPoint _phaseOrigin;
    NSPoint _autonomyTarget;
    NSPoint _pointerOffset;
    NSPoint _lastPointerPoint;
    NSTimeInterval _returnDuration;
    NSTimeInterval _autonomyDuration;
    NSTimeInterval _lastInteractionAt;
    NSTimeInterval _nextExploreAt;
    NSTimeInterval _edgeRestUntil;
    uint32_t _autonomyRandomState;
    NSUInteger _explorationsSinceRest;
    CGFloat _boundaryTargetX;
    CGFloat _boundaryTargetY;
    BOOL _workQuiet;
    BOOL _returnStartedPublished;
}

- (instancetype)init
{
    self = [super init];
    if (self != nil) {
        NSRect frame = NSMakeRect(0.0, 0.0, FridayOrbSize, FridayOrbSize);
        _panel = [[NSPanel alloc] initWithContentRect:frame
                                           styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
        _panel.opaque = NO;
        _panel.backgroundColor = NSColor.clearColor;
        _panel.hasShadow = YES;
        _panel.level = NSFloatingWindowLevel;
        _panel.hidesOnDeactivate = NO;
        _panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                    NSWindowCollectionBehaviorFullScreenAuxiliary |
                                    NSWindowCollectionBehaviorStationary;
        _panel.ignoresMouseEvents = NO;
        _panel.acceptsMouseMovedEvents = YES;

        _view = [[FridayOverlayView alloc] initWithFrame:frame];
        _panel.contentView = _view;
        __weak FridayOverlayController *weakSelf = self;
        _view.onPointerDown = ^(NSPoint point) {
            [weakSelf pointerDown:point];
        };
        _view.onPointerDragged = ^(NSPoint point) {
            [weakSelf pointerDragged:point];
        };
        _view.onPointerUp = ^(NSPoint point, BOOL dragged) {
            [weakSelf pointerUp:point dragged:dragged];
        };
        _phase = FridayOverlayPhaseHidden;
        _workQuiet = YES;
        _autonomyRandomState = 0x46524944U;  // "FRID"
        _timer = [NSTimer timerWithTimeInterval:1.0 / 60.0
                                         target:self
                                       selector:@selector(tick:)
                                       userInfo:nil
                                        repeats:YES];
        [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
    }
    return self;
}

- (BOOL)isPresent
{
    return _phase != FridayOverlayPhaseHidden;
}

- (void)setWorkQuiet:(BOOL)workQuiet
{
    if (_workQuiet == workQuiet) {
        return;
    }
    _workQuiet = workQuiet;
    if (_phase == FridayOverlayPhaseHidden) {
        return;
    }

    NSTimeInterval now = NSDate.timeIntervalSinceReferenceDate;
    if (_workQuiet) {
        // Do not abruptly cancel a gesture already in flight; simply make the
        // following pause longer once the current movement has settled.
        _nextExploreAt = fmax(_nextExploreAt, now + 24.0);
    } else if (_phase == FridayOverlayPhaseIdle) {
        _nextExploreAt = fmin(_nextExploreAt, now + 9.0 + RandomUnit([self nextAutonomyRandom]) * 7.0);
    }
}

- (uint32_t)nextAutonomyRandom
{
    uint32_t value = _autonomyRandomState;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    _autonomyRandomState = value == 0U ? 0x46524944U : value;
    return _autonomyRandomState;
}

- (void)scheduleNextExploreFrom:(NSTimeInterval)now
{
    CGFloat random = RandomUnit([self nextAutonomyRandom]);
    // Active work gets a much longer quiet window. When input has stopped,
    // Friday is allowed to become a little more curious without ever crossing
    // to another display on its own.
    NSTimeInterval delay = _workQuiet ? 26.0 + random * 20.0 : 12.0 + random * 16.0;
    _nextExploreAt = now + delay;
}

- (void)clearBoundaryPose
{
    _view.boundaryX = 0.0;
    _view.boundaryY = 0.0;
    _view.boundaryStrength = 0.0;
    _view.restingAtBoundary = NO;
}

- (void)beginExplorationAt:(NSTimeInterval)now
{
    NSRect panelFrame = _panel.frame;
    NSPoint centre = NSMakePoint(NSMidX(panelFrame), NSMidY(panelFrame));
    NSScreen *screen = [self screenContainingPoint:centre];
    if (screen == nil) {
        [self scheduleNextExploreFrom:now];
        return;
    }

    NSRect visible = screen.visibleFrame;
    CGFloat left = NSMinX(visible) + 24.0;
    CGFloat right = NSMaxX(visible) - FridayOrbSize - 24.0;
    CGFloat bottom = NSMinY(visible) + 24.0;
    CGFloat top = NSMaxY(visible) - FridayOrbSize - 24.0;
    if (right < left || top < bottom) {
        [self scheduleNextExploreFrom:now];
        return;
    }

    NSPoint current = panelFrame.origin;
    CGFloat distances[4] = {
        fabs(current.x - left),
        fabs(current.x - right),
        fabs(current.y - bottom),
        fabs(current.y - top),
    };
    NSUInteger nearestSide = 0;
    for (NSUInteger side = 1; side < 4; ++side) {
        if (distances[side] < distances[nearestSide]) {
            nearestSide = side;
        }
    }

    NSUInteger side = nearestSide;
    if (!_workQuiet && RandomUnit([self nextAutonomyRandom]) > 0.72) {
        side = [self nextAutonomyRandom] % 4U;
    }

    CGFloat xRange = right - left;
    CGFloat yRange = top - bottom;
    NSPoint target = current;
    if (side == 0U || side == 1U) {
        target.x = side == 0U ? left : right;
        target.y = bottom + RandomUnit([self nextAutonomyRandom]) * yRange;
    } else {
        target.x = left + RandomUnit([self nextAutonomyRandom]) * xRange;
        target.y = side == 2U ? bottom : top;
    }

    // Avoid parking over the area the user is currently manipulating. The
    // pointer still controls Friday's gaze, but autonomous locomotion chooses
    // the opposite half of the same edge when it would come too close.
    NSPoint pointer = NSEvent.mouseLocation;
    NSPoint targetCentre = NSMakePoint(target.x + FridayOrbSize * 0.5,
                                       target.y + FridayOrbSize * 0.5);
    if (hypot(pointer.x - targetCentre.x, pointer.y - targetCentre.y) < 220.0) {
        if (side == 0U || side == 1U) {
            target.y = pointer.y >= NSMidY(visible) ? bottom : top;
        } else {
            target.x = pointer.x >= NSMidX(visible) ? left : right;
        }
    }

    // While the user is active, exploration stays on the nearest edge and is
    // deliberately short. Longer idle periods permit a wider patrol.
    CGFloat maximumTravel = _workQuiet ? 300.0 : 560.0;
    target.x = Clamp(target.x, current.x - maximumTravel, current.x + maximumTravel);
    target.y = Clamp(target.y, current.y - maximumTravel, current.y + maximumTravel);
    target.x = Clamp(target.x, left, right);
    target.y = Clamp(target.y, bottom, top);

    CGFloat dx = target.x - current.x;
    CGFloat dy = target.y - current.y;
    CGFloat distance = hypot(dx, dy);
    if (distance < 72.0) {
        if (side == 0U || side == 1U) {
            target.y = Clamp(current.y + (RandomUnit([self nextAutonomyRandom]) < 0.5 ? -150.0 : 150.0), bottom, top);
        } else {
            target.x = Clamp(current.x + (RandomUnit([self nextAutonomyRandom]) < 0.5 ? -150.0 : 150.0), left, right);
        }
        dx = target.x - current.x;
        dy = target.y - current.y;
        distance = hypot(dx, dy);
    }

    _phaseOrigin = current;
    _autonomyTarget = target;
    _autonomyDuration = Clamp(1.05 + distance / 520.0, 1.15, _workQuiet ? 2.05 : 2.65);
    _phaseStartedAt = now;
    _view.autonomousGazeX = distance > 1.0 ? Clamp(dx / distance, -1.0, 1.0) * 0.72 : 0.0;
    _view.autonomousGazeY = distance > 1.0 ? Clamp(dy / distance, -1.0, 1.0) * 0.58 : 0.0;
    [self clearBoundaryPose];
    _phase = FridayOverlayPhaseExploring;
    puts("[Autonomy] Friday is quietly exploring the display edge");
}

- (void)beginEdgeRestAt:(NSTimeInterval)now
{
    NSRect panelFrame = _panel.frame;
    NSPoint centre = NSMakePoint(NSMidX(panelFrame), NSMidY(panelFrame));
    NSScreen *screen = [self screenContainingPoint:centre];
    if (screen == nil) {
        [self scheduleNextExploreFrom:now];
        return;
    }

    // Use the physical screen frame here rather than visibleFrame: part of the
    // orb must actually leave the display for the bezel to clip a flat contact
    // surface into Friday's soft silhouette. An edge shared with another Mac
    // display is not a physical boundary, so never tuck Friday through it.
    NSRect screenFrame = screen.frame;
    BOOL leftIsOuter = YES;
    BOOL rightIsOuter = YES;
    BOOL bottomIsOuter = YES;
    BOOL topIsOuter = YES;
    for (NSScreen *other in NSScreen.screens) {
        if (other == screen) {
            continue;
        }
        NSRect otherFrame = other.frame;
        CGFloat verticalOverlap = fmin(NSMaxY(screenFrame), NSMaxY(otherFrame)) -
                                  fmax(NSMinY(screenFrame), NSMinY(otherFrame));
        CGFloat horizontalOverlap = fmin(NSMaxX(screenFrame), NSMaxX(otherFrame)) -
                                    fmax(NSMinX(screenFrame), NSMinX(otherFrame));
        if (verticalOverlap > 2.0 && fabs(NSMaxX(otherFrame) - NSMinX(screenFrame)) <= 2.0) {
            leftIsOuter = NO;
        }
        if (verticalOverlap > 2.0 && fabs(NSMinX(otherFrame) - NSMaxX(screenFrame)) <= 2.0) {
            rightIsOuter = NO;
        }
        if (horizontalOverlap > 2.0 && fabs(NSMaxY(otherFrame) - NSMinY(screenFrame)) <= 2.0) {
            bottomIsOuter = NO;
        }
        if (horizontalOverlap > 2.0 && fabs(NSMinY(otherFrame) - NSMaxY(screenFrame)) <= 2.0) {
            topIsOuter = NO;
        }
    }
    if (!leftIsOuter && !rightIsOuter && !bottomIsOuter && !topIsOuter) {
        [self scheduleNextExploreFrom:now];
        return;
    }

    BOOL useLeft = fabs(NSMidX(panelFrame) - NSMinX(screenFrame)) <=
                   fabs(NSMaxX(screenFrame) - NSMidX(panelFrame));
    BOOL useBottom = fabs(NSMidY(panelFrame) - NSMinY(screenFrame)) <=
                     fabs(NSMaxY(screenFrame) - NSMidY(panelFrame));
    if (useLeft && !leftIsOuter && rightIsOuter) {
        useLeft = NO;
    } else if (!useLeft && !rightIsOuter && leftIsOuter) {
        useLeft = YES;
    }
    if (useBottom && !bottomIsOuter && topIsOuter) {
        useBottom = NO;
    } else if (!useBottom && !topIsOuter && bottomIsOuter) {
        useBottom = YES;
    }

    BOOL contactHorizontal = useLeft ? leftIsOuter : rightIsOuter;
    BOOL contactVertical = useBottom ? bottomIsOuter : topIsOuter;
    _boundaryTargetX = contactHorizontal ? (useLeft ? -1.0 : 1.0) : 0.0;
    // FridayOverlayView is flipped, so the bottom screen edge is positive Y in
    // its local drawing coordinates and the top edge is negative.
    _boundaryTargetY = contactVertical ? (useBottom ? 1.0 : -1.0) : 0.0;

    _phaseOrigin = panelFrame.origin;
    CGFloat targetX = useLeft ? NSMinX(screenFrame) - (contactHorizontal ? FridayEdgeTuck : -24.0)
                              : NSMaxX(screenFrame) - FridayOrbSize + (contactHorizontal ? FridayEdgeTuck : -24.0);
    CGFloat targetY = useBottom ? NSMinY(screenFrame) - (contactVertical ? FridayEdgeTuck : -24.0)
                                : NSMaxY(screenFrame) - FridayOrbSize + (contactVertical ? FridayEdgeTuck : -24.0);
    _autonomyTarget = NSMakePoint(targetX, targetY);
    CGFloat dx = _autonomyTarget.x - _phaseOrigin.x;
    CGFloat dy = _autonomyTarget.y - _phaseOrigin.y;
    CGFloat distance = hypot(dx, dy);
    _autonomyDuration = Clamp(1.25 + distance / 650.0, 1.35, 2.70);
    _phaseStartedAt = now;
    _view.autonomousGazeX = distance > 1.0 ? Clamp(dx / distance, -1.0, 1.0) * 0.62 : 0.0;
    _view.autonomousGazeY = distance > 1.0 ? Clamp(dy / distance, -1.0, 1.0) * 0.50 : 0.0;
    _phase = FridayOverlayPhaseNesting;
    puts("[Autonomy] Friday is curling up against a display corner");
}

- (void)wakeFromEdgeAt:(NSTimeInterval)now
{
    _phaseOrigin = _panel.frame.origin;
    _autonomyTarget = NSMakePoint(_phaseOrigin.x - _boundaryTargetX * (FridayEdgeTuck + 30.0),
                                  _phaseOrigin.y + _boundaryTargetY * (FridayEdgeTuck + 30.0));
    _autonomyDuration = 1.08;
    _phaseStartedAt = now;
    _view.restingAtBoundary = NO;
    _view.autonomousGazeX = -_boundaryTargetX * 0.55;
    _view.autonomousGazeY = -_boundaryTargetY * 0.35;
    _phase = FridayOverlayPhaseWaking;
}

- (NSScreen *)portalScreenForDirection:(FridayTravelDirection)direction
{
    NSScreen *chosen = NSScreen.mainScreen ?: NSScreen.screens.firstObject;
    for (NSScreen *candidate in NSScreen.screens) {
        if (chosen == nil) {
            chosen = candidate;
        } else if (direction == FridayTravelDirectionLeft &&
                   NSMaxX(candidate.visibleFrame) > NSMaxX(chosen.visibleFrame)) {
            chosen = candidate;
        } else if (direction == FridayTravelDirectionRight &&
                   NSMinX(candidate.visibleFrame) < NSMinX(chosen.visibleFrame)) {
            chosen = candidate;
        } else if (direction == FridayTravelDirectionUp &&
                   NSMinY(candidate.visibleFrame) < NSMinY(chosen.visibleFrame)) {
            chosen = candidate;
        } else if (direction == FridayTravelDirectionDown &&
                   NSMaxY(candidate.visibleFrame) > NSMaxY(chosen.visibleFrame)) {
            chosen = candidate;
        }
    }
    return chosen;
}

- (NSScreen *)screenContainingPoint:(NSPoint)point
{
    for (NSScreen *screen in NSScreen.screens) {
        if (NSPointInRect(point, screen.frame)) {
            return screen;
        }
    }
    return NSScreen.mainScreen ?: NSScreen.screens.firstObject;
}

- (void)configurePortalOrigins
{
    NSScreen *screen = [self portalScreenForDirection:_direction];
    if (screen == nil) {
        return;
    }
    NSRect visible = screen.visibleFrame;
    CGFloat targetY = NSMinY(visible) + NSHeight(visible) * 0.62 - FridayOrbSize * 0.5;
    targetY = Clamp(targetY, NSMinY(visible) + 24.0, NSMaxY(visible) - FridayOrbSize - 24.0);

    if (_direction == FridayTravelDirectionLeft) {
        _hiddenOrigin = NSMakePoint(NSMaxX(visible) - FridayOrbSize * 0.04, targetY);
        _peekOrigin = NSMakePoint(NSMaxX(visible) - FridayOrbSize * 0.42, targetY);
        _shyOrigin = NSMakePoint(NSMaxX(visible) - FridayOrbSize * 0.20, targetY);
        _restOrigin = NSMakePoint(NSMaxX(visible) - FridayOrbSize - 28.0, targetY);
    } else if (_direction == FridayTravelDirectionRight) {
        _hiddenOrigin = NSMakePoint(NSMinX(visible) - FridayOrbSize * 0.96, targetY);
        _peekOrigin = NSMakePoint(NSMinX(visible) - FridayOrbSize * 0.58, targetY);
        _shyOrigin = NSMakePoint(NSMinX(visible) - FridayOrbSize * 0.80, targetY);
        _restOrigin = NSMakePoint(NSMinX(visible) + 28.0, targetY);
    } else {
        // Vertical portals are reserved by the packet format and use a simple
        // centred fallback until the StopWatch supports vertical edge drags.
        CGFloat targetX = NSMidX(visible) - FridayOrbSize * 0.5;
        BOOL fromBottom = _direction == FridayTravelDirectionUp;
        CGFloat edge = fromBottom ? NSMinY(visible) : NSMaxY(visible);
        _hiddenOrigin = NSMakePoint(targetX, edge + (fromBottom ? -FridayOrbSize * 0.96 : -FridayOrbSize * 0.04));
        _peekOrigin = NSMakePoint(targetX, edge + (fromBottom ? -FridayOrbSize * 0.58 : -FridayOrbSize * 0.42));
        _shyOrigin = NSMakePoint(targetX, edge + (fromBottom ? -FridayOrbSize * 0.80 : -FridayOrbSize * 0.20));
        _restOrigin = NSMakePoint(targetX, fromBottom ? NSMinY(visible) + 28.0 : NSMaxY(visible) - FridayOrbSize - 28.0);
    }
}

- (void)presentFromDirection:(FridayTravelDirection)direction
                        seed:(uint16_t)seed
                    sequence:(uint8_t)sequence
{
    _direction = direction;
    _sequence = sequence;
    _view.seed = seed;
    _view.autonomousGazeX = 0.0;
    _view.autonomousGazeY = 0.0;
    [self clearBoundaryPose];
    _returnStartedPublished = NO;
    [self configurePortalOrigins];
    [_panel setFrameOrigin:_hiddenOrigin];
    _panel.alphaValue = 0.0;
    [_panel orderFrontRegardless];
    _phaseStartedAt = NSDate.timeIntervalSinceReferenceDate;
    _visibleStartedAt = _phaseStartedAt;
    _lastInteractionAt = _phaseStartedAt;
    _autonomyRandomState = ((uint32_t)seed << 16U) ^ (uint32_t)sequence ^ 0x46524944U;
    if (_autonomyRandomState == 0U) {
        _autonomyRandomState = 0x46524944U;
    }
    _explorationsSinceRest = 0;
    [self scheduleNextExploreFrom:_phaseStartedAt];
    _phase = FridayOverlayPhasePeeking;
    printf("[Travel] Friday is cautiously peeking onto the Mac (sequence %u)\n", sequence);
}

- (void)pointerDown:(NSPoint)point
{
    if (_phase != FridayOverlayPhaseIdle && _phase != FridayOverlayPhaseSettling &&
        _phase != FridayOverlayPhaseExploring && _phase != FridayOverlayPhaseNesting &&
        _phase != FridayOverlayPhaseEdgeResting && _phase != FridayOverlayPhaseWaking) {
        return;
    }
    NSTimeInterval now = NSDate.timeIntervalSinceReferenceDate;
    _lastInteractionAt = now;
    _explorationsSinceRest = 0;
    [self scheduleNextExploreFrom:now];

    if (_phase == FridayOverlayPhaseExploring || _phase == FridayOverlayPhaseNesting ||
        _phase == FridayOverlayPhaseEdgeResting || _phase == FridayOverlayPhaseWaking) {
        // Human attention always wins. Stop the autonomous performance at its
        // current frame and bring a tucked Friday just inside the work area.
        _phaseOrigin = _panel.frame.origin;
        _restOrigin = [self clampedRestOriginNearPoint:point];
        _phaseStartedAt = now;
        _phase = FridayOverlayPhaseSettling;
        _view.autonomousGazeX = 0.0;
        _view.autonomousGazeY = 0.0;
        [self clearBoundaryPose];
    }
    _pointerOffset = NSMakePoint(point.x - _panel.frame.origin.x,
                                 point.y - _panel.frame.origin.y);
    _lastPointerPoint = point;
}

- (void)pointerDragged:(NSPoint)point
{
    if (_phase != FridayOverlayPhaseIdle && _phase != FridayOverlayPhaseSettling &&
        _phase != FridayOverlayPhaseDragging) {
        return;
    }
    _phase = FridayOverlayPhaseDragging;
    _view.beingDragged = YES;
    _view.autonomousGazeX = 0.0;
    _view.autonomousGazeY = 0.0;
    [self clearBoundaryPose];
    _view.dragEnergy = hypot(point.x - _lastPointerPoint.x, point.y - _lastPointerPoint.y);
    _lastPointerPoint = point;
    NSPoint origin = NSMakePoint(point.x - _pointerOffset.x, point.y - _pointerOffset.y);
    [_panel setFrameOrigin:origin];
}

- (BOOL)isPointAtPortal:(NSPoint)point
{
    NSScreen *portal = [self portalScreenForDirection:_direction];
    if (portal == nil) {
        return NO;
    }
    NSRect visible = portal.visibleFrame;
    if (_direction == FridayTravelDirectionLeft) {
        return NSMaxX(visible) - point.x <= 86.0;
    }
    if (_direction == FridayTravelDirectionRight) {
        return point.x - NSMinX(visible) <= 86.0;
    }
    if (_direction == FridayTravelDirectionUp) {
        return point.y - NSMinY(visible) <= 86.0;
    }
    return NSMaxY(visible) - point.y <= 86.0;
}

- (NSPoint)clampedRestOriginNearPoint:(NSPoint)point
{
    NSScreen *screen = [self screenContainingPoint:point];
    if (screen == nil) {
        return _panel.frame.origin;
    }
    NSRect visible = screen.visibleFrame;
    return NSMakePoint(Clamp(_panel.frame.origin.x, NSMinX(visible) + 18.0,
                             NSMaxX(visible) - FridayOrbSize - 18.0),
                       Clamp(_panel.frame.origin.y, NSMinY(visible) + 18.0,
                             NSMaxY(visible) - FridayOrbSize - 18.0));
}

- (void)pointerUp:(NSPoint)point dragged:(BOOL)dragged
{
    if (!dragged) {
        if (_phase == FridayOverlayPhaseIdle || _phase == FridayOverlayPhaseSettling) {
            [_view playClickReaction];
            printf("[Interaction] Friday noticed a click\n");
        }
        return;
    }
    if (_phase != FridayOverlayPhaseDragging) {
        return;
    }
    _view.beingDragged = NO;
    _view.dragEnergy = 0.0;
    if ([self isPointAtPortal:point]) {
        [self returnToDevice];
        return;
    }
    _phaseOrigin = _panel.frame.origin;
    _restOrigin = [self clampedRestOriginNearPoint:point];
    _phaseStartedAt = NSDate.timeIntervalSinceReferenceDate;
    _lastInteractionAt = _phaseStartedAt;
    _explorationsSinceRest = 0;
    [self scheduleNextExploreFrom:_phaseStartedAt];
    _phase = FridayOverlayPhaseSettling;
    printf("[Interaction] Friday moved to another desk position\n");
}

- (void)returnToDevice
{
    if (_phase == FridayOverlayPhaseHidden || _phase == FridayOverlayPhaseReturning) {
        return;
    }
    _phaseOrigin = _panel.frame.origin;
    [self configurePortalOrigins];
    _phaseStartedAt = NSDate.timeIntervalSinceReferenceDate;
    _phase = FridayOverlayPhaseReturning;
    _view.autonomousGazeX = 0.0;
    _view.autonomousGazeY = 0.0;
    [self clearBoundaryPose];
    CGFloat travelDistance = hypot(_phaseOrigin.x - _hiddenOrigin.x,
                                   _phaseOrigin.y - _hiddenOrigin.y);
    _returnDuration = Clamp(1.18 + travelDistance / 1500.0, 1.22, 2.45);
    _view.beingDragged = NO;
    _view.dragEnergy = 0.0;
    if (!_returnStartedPublished) {
        _returnStartedPublished = YES;
        if (self.onReturnStarted != nil) {
            self.onReturnStarted(_direction, _sequence);
        }
    }
    printf("[Travel] Friday is sneaking back to StopWatch (sequence %u)\n", _sequence);
}

- (void)dismissImmediately
{
    _phase = FridayOverlayPhaseHidden;
    _view.beingDragged = NO;
    _view.autonomousGazeX = 0.0;
    _view.autonomousGazeY = 0.0;
    [self clearBoundaryPose];
    [_panel orderOut:nil];
}

- (void)finishReturn
{
    FridayTravelDirection direction = _direction;
    uint8_t sequence = _sequence;
    [self dismissImmediately];
    // The completion callback is the visibility barrier: orderOut happens
    // before HostHidden is written to the StopWatch.
    if (self.onReturnCompleted != nil) {
        self.onReturnCompleted(direction, sequence);
    }
}

- (void)tick:(NSTimer *)timer
{
    (void)timer;
    if (_phase == FridayOverlayPhaseHidden) {
        return;
    }

    NSTimeInterval now = NSDate.timeIntervalSinceReferenceDate;
    _view.animationTime = now - _visibleStartedAt;
    NSTimeInterval elapsed = now - _phaseStartedAt;
    if (_phase == FridayOverlayPhasePeeking) {
        CGFloat progress = SmoothStep(elapsed / 0.58);
        [_panel setFrameOrigin:InterpolatePoint(_hiddenOrigin, _peekOrigin, progress)];
        _panel.alphaValue = SmoothStep(progress * 2.2);
        if (progress >= 0.999) {
            _phase = FridayOverlayPhaseWatching;
            _phaseStartedAt = now;
        }
    } else if (_phase == FridayOverlayPhaseWatching) {
        // Hold a half-visible silhouette long enough for the user to notice
        // that Friday is checking whether the new screen is safe.
        CGFloat curiousBob = sin(elapsed * 2.0 * M_PI / 0.72) * 2.0;
        [_panel setFrameOrigin:NSMakePoint(_peekOrigin.x, _peekOrigin.y + curiousBob)];
        if (elapsed >= 0.82) {
            _phaseOrigin = _panel.frame.origin;
            _phase = FridayOverlayPhaseRetreating;
            _phaseStartedAt = now;
        }
    } else if (_phase == FridayOverlayPhaseRetreating) {
        CGFloat progress = SmoothStep(elapsed / 0.36);
        [_panel setFrameOrigin:InterpolatePoint(_phaseOrigin, _shyOrigin, progress)];
        if (progress >= 0.999) {
            _phase = FridayOverlayPhaseEntering;
            _phaseStartedAt = now;
        }
    } else if (_phase == FridayOverlayPhaseEntering) {
        CGFloat phase = Clamp(elapsed / 1.34, 0.0, 1.0);
        CGFloat progress;
        if (phase < 0.28) {
            progress = 0.32 * SmoothStep(phase / 0.28);
        } else if (phase < 0.43) {
            progress = 0.32;
        } else if (phase < 0.74) {
            progress = 0.32 + 0.39 * SmoothStep((phase - 0.43) / 0.31);
        } else {
            progress = 0.71 + 0.29 * SmoothStep((phase - 0.74) / 0.26);
        }
        NSPoint point = InterpolatePoint(_shyOrigin, _restOrigin, progress);
        point.y += sin(phase * 3.0 * M_PI) * (1.0 - phase) * 3.0;
        [_panel setFrameOrigin:point];
        if (phase >= 0.999) {
            _phase = FridayOverlayPhaseIdle;
            _phaseStartedAt = now;
        }
    } else if (_phase == FridayOverlayPhaseIdle) {
        CGFloat floatY = sin((now - _visibleStartedAt) * 2.0 * M_PI / 3.4) * 3.0;
        [_panel setFrameOrigin:NSMakePoint(_restOrigin.x, _restOrigin.y + floatY)];
        if (now >= _nextExploreAt) {
            NSTimeInterval restAfter = _workQuiet ? 78.0 : 52.0;
            if (_explorationsSinceRest >= 2U && now - _lastInteractionAt >= restAfter) {
                [self beginEdgeRestAt:now];
            } else {
                [self beginExplorationAt:now];
            }
        }
    } else if (_phase == FridayOverlayPhaseSettling) {
        CGFloat phase = Clamp(elapsed / 0.68, 0.0, 1.0);
        CGFloat progress = SmoothStep(phase);
        NSPoint point = InterpolatePoint(_phaseOrigin, _restOrigin, progress);
        point.y += sin(phase * 2.0 * M_PI) * (1.0 - phase) * 7.0;
        [_panel setFrameOrigin:point];
        if (phase >= 0.999) {
            _phase = FridayOverlayPhaseIdle;
            _phaseStartedAt = now;
        }
    } else if (_phase == FridayOverlayPhaseExploring) {
        CGFloat phase = Clamp(elapsed / _autonomyDuration, 0.0, 1.0);
        // Two soft scurries with a tiny observational pause in the middle feel
        // intentional without turning a work overlay into constant motion.
        CGFloat progress;
        if (phase < 0.44) {
            progress = 0.58 * SmoothStep(phase / 0.44);
        } else if (phase < 0.56) {
            progress = 0.58;
        } else {
            progress = 0.58 + 0.42 * SmoothStep((phase - 0.56) / 0.44);
        }
        NSPoint point = InterpolatePoint(_phaseOrigin, _autonomyTarget, progress);
        CGFloat dx = _autonomyTarget.x - _phaseOrigin.x;
        CGFloat dy = _autonomyTarget.y - _phaseOrigin.y;
        CGFloat distance = fmax(1.0, hypot(dx, dy));
        CGFloat arc = sin(phase * M_PI) * (_workQuiet ? 6.0 : 11.0);
        point.x += -dy / distance * arc;
        point.y += dx / distance * arc;
        [_panel setFrameOrigin:point];
        if (phase >= 0.999) {
            _restOrigin = _autonomyTarget;
            _view.autonomousGazeX = 0.0;
            _view.autonomousGazeY = 0.0;
            ++_explorationsSinceRest;
            [self scheduleNextExploreFrom:now];
            _phaseStartedAt = now;
            _phase = FridayOverlayPhaseIdle;
        }
    } else if (_phase == FridayOverlayPhaseNesting) {
        CGFloat phase = Clamp(elapsed / _autonomyDuration, 0.0, 1.0);
        CGFloat progress = SmoothStep(phase);
        NSPoint point = InterpolatePoint(_phaseOrigin, _autonomyTarget, progress);
        CGFloat tuckWiggle = sin(phase * 3.0 * M_PI) * (1.0 - phase) * 4.0;
        point.x += _boundaryTargetX * tuckWiggle;
        point.y -= _boundaryTargetY * tuckWiggle * 0.65;
        [_panel setFrameOrigin:point];
        _view.boundaryX = _boundaryTargetX;
        _view.boundaryY = _boundaryTargetY;
        _view.boundaryStrength = SmoothStep((phase - 0.54) / 0.46);
        if (phase >= 0.999) {
            _restOrigin = _autonomyTarget;
            _view.autonomousGazeX = 0.0;
            _view.autonomousGazeY = 0.0;
            _view.boundaryStrength = 1.0;
            _view.restingAtBoundary = YES;
            _edgeRestUntil = now + (_workQuiet ? 38.0 : 24.0) +
                             RandomUnit([self nextAutonomyRandom]) * (_workQuiet ? 24.0 : 16.0);
            _phaseStartedAt = now;
            _phase = FridayOverlayPhaseEdgeResting;
        }
    } else if (_phase == FridayOverlayPhaseEdgeResting) {
        // Slow pressure changes sell the idea that the orb has weight and that
        // the display edge is pushing back against a soft body.
        CGFloat pressure = sin(elapsed * 2.0 * M_PI / 4.6) * 1.35;
        [_panel setFrameOrigin:NSMakePoint(_restOrigin.x + _boundaryTargetX * pressure,
                                           _restOrigin.y - _boundaryTargetY * pressure * 0.65)];
        if (now >= _edgeRestUntil) {
            [self wakeFromEdgeAt:now];
        }
    } else if (_phase == FridayOverlayPhaseWaking) {
        CGFloat phase = Clamp(elapsed / _autonomyDuration, 0.0, 1.0);
        CGFloat progress = SmoothStep(phase);
        CGFloat spring = sin(phase * 2.0 * M_PI) * (1.0 - phase) * 5.0;
        NSPoint point = InterpolatePoint(_phaseOrigin, _autonomyTarget, progress);
        point.x -= _boundaryTargetX * spring;
        point.y += _boundaryTargetY * spring * 0.65;
        [_panel setFrameOrigin:point];
        _view.boundaryStrength = 1.0 - SmoothStep(phase / 0.78);
        if (phase >= 0.999) {
            _restOrigin = _autonomyTarget;
            _view.autonomousGazeX = 0.0;
            _view.autonomousGazeY = 0.0;
            [self clearBoundaryPose];
            _explorationsSinceRest = 0;
            _lastInteractionAt = now;
            [self scheduleNextExploreFrom:now];
            _phaseStartedAt = now;
            _phase = FridayOverlayPhaseIdle;
        }
    } else if (_phase == FridayOverlayPhaseReturning) {
        CGFloat phase = Clamp(elapsed / _returnDuration, 0.0, 1.0);
        CGFloat movement = phase < 0.20 ? 0.0 : SmoothStep((phase - 0.20) / 0.80);
        // A short backward glance precedes the retreat, then the body leaves
        // in two soft shuffles rather than one linear slide.
        if (movement > 0.0 && movement < 0.62) {
            movement *= 0.88 + 0.12 * sin(movement * 4.0 * M_PI);
        }
        [_panel setFrameOrigin:InterpolatePoint(_phaseOrigin, _hiddenOrigin, movement)];
        _panel.alphaValue = 1.0 - SmoothStep((phase - 0.82) / 0.18);
        if (phase >= 0.999) {
            [self finishReturn];
        }
    }
    [_view setNeedsDisplay:YES];
}

@end
