// SPDX-License-Identifier: MIT
#import <Foundation/Foundation.h>

NSString *FindCodexExecutable(void);
// Reads only account/rateLimits/read using a short-lived app-server child.
NSData *ReadCodexQuota(NSString *executable, NSError **error);
NSData *QuotaDataFromResult(NSDictionary *result, NSTimeInterval now, NSError **error);
BOOL TestQuotaEncoding(void);
