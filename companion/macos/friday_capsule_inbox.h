/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#import <AppKit/AppKit.h>

@class FridayCapsuleStore;

@interface FridayCapsuleInboxController : NSObject <NSTableViewDataSource, NSTableViewDelegate>
- (instancetype)initWithStore:(FridayCapsuleStore *)store;
- (void)refresh;
@end
