/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#import <Foundation/Foundation.h>

typedef NS_ENUM(uint8_t, FridayCapsuleFeedbackState) {
    FridayCapsuleFeedbackIdle,
    FridayCapsuleFeedbackRecording,
    FridayCapsuleFeedbackAwaitingSave,
    FridayCapsuleFeedbackSaved,
    FridayCapsuleFeedbackFailed,
};

@interface FridayCapsuleStore : NSObject
@property(nonatomic, copy) void (^sendAcknowledgement)(NSData *data);
@property(nonatomic, copy) void (^onFeedback)(FridayCapsuleFeedbackState state, CGFloat progress);
@property(nonatomic, copy) void (^onItemsChanged)(void);
- (void)handlePacket:(NSData *)data;
- (void)abortActiveCapture;
- (void)expireStaleCapture;
- (NSArray<NSDictionary *> *)itemsForToday;
- (NSURL *)audioURLForItem:(NSDictionary *)item;
- (void)toggleCompletedForItemID:(NSString *)itemID;
@end
