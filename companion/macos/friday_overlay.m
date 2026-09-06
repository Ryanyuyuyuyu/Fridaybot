/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#import "friday_overlay.h"
#include <math.h>
#include <stdio.h>

static const CGFloat FridayOrbSize = 168.0;

typedef NS_ENUM(uint8_t, FridayOverlayPhase) {
    FridayOverlayPhaseHidden,
    FridayOverlayPhasePeeking,
    FridayOverlayPhaseWatching,
    FridayOverlayPhaseRetreating,
    FridayOverlayPhaseEntering,
    FridayOverlayPhaseIdle,
    FridayOverlayPhaseDragging,
    FridayOverlayPhaseSettling,
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

@interface FridayOverlayView : NSView
@property(nonatomic) CGFloat animationTime;
@property(nonatomic) NSTimeInterval reactionStartedAt;
@property(nonatomic) uint16_t seed;
@property(nonatomic) BOOL beingDragged;
@property(nonatomic) CGFloat dragEnergy;
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

    NSRect orbRect = NSInsetRect(self.bounds, 6.0, 6.0);
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
    CGFloat time = self.animationTime;
    CGFloat bob = self.beingDragged ? 0.0 : sin(time * 2.0 * M_PI / 2.8) * 2.2;

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
    leftHeight = fmax(5.0, leftHeight * blink);
    rightHeight = fmax(5.0, rightHeight * blink);

    CGFloat centreX = NSMidX(self.bounds) + gazeX * 9.0;
    CGFloat centreY = NSMidY(self.bounds) - gazeY * 6.0 - 7.0 + bob;
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
    NSPoint _pointerOffset;
    NSPoint _lastPointerPoint;
    NSTimeInterval _returnDuration;
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
    _returnStartedPublished = NO;
    [self configurePortalOrigins];
    [_panel setFrameOrigin:_hiddenOrigin];
    _panel.alphaValue = 0.0;
    [_panel orderFrontRegardless];
    _phaseStartedAt = NSDate.timeIntervalSinceReferenceDate;
    _visibleStartedAt = _phaseStartedAt;
    _phase = FridayOverlayPhasePeeking;
    printf("[Travel] Friday is cautiously peeking onto the Mac (sequence %u)\n", sequence);
}

- (void)pointerDown:(NSPoint)point
{
    if (_phase != FridayOverlayPhaseIdle && _phase != FridayOverlayPhaseSettling) {
        return;
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
        }
    } else if (_phase == FridayOverlayPhaseIdle) {
        CGFloat floatY = sin((now - _visibleStartedAt) * 2.0 * M_PI / 3.4) * 3.0;
        [_panel setFrameOrigin:NSMakePoint(_restOrigin.x, _restOrigin.y + floatY)];
    } else if (_phase == FridayOverlayPhaseSettling) {
        CGFloat phase = Clamp(elapsed / 0.68, 0.0, 1.0);
        CGFloat progress = SmoothStep(phase);
        NSPoint point = InterpolatePoint(_phaseOrigin, _restOrigin, progress);
        point.y += sin(phase * 2.0 * M_PI) * (1.0 - phase) * 7.0;
        [_panel setFrameOrigin:point];
        if (phase >= 0.999) {
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
