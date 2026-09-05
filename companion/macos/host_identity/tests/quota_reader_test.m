// SPDX-License-Identifier: MIT
#import "../quota_reader.h"

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        if (argc != 2) return 2;
        NSString *fixture = [NSString stringWithUTF8String:argv[1]];
        for (NSString *scenario in @[@"normal", @"initialize_error", @"read_error", @"closed_pipe"]) {
            setenv("CODEX_IDENTITY_TEST_SCENARIO", scenario.UTF8String, 1);
            NSError *error = nil;
            NSData *data = ReadCodexQuota(fixture, &error);
            if ([scenario isEqual:@"normal"]) {
                NSDictionary *decoded = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
                if (!decoded || error || ![decoded[@"remaining_percent"] isEqual:@74.5] ||
                    ![decoded[@"weekly_used_percent"] isEqual:@80] || ![decoded[@"reset_in_seconds"] isEqual:@0]) {
                    fprintf(stderr, "Fake app-server normal response failed.\n");
                    return 1;
                }
            } else if (data || !error) {
                fprintf(stderr, "Fake app-server error handling failed.\n");
                return 1;
            }
        }
        unsetenv("CODEX_IDENTITY_TEST_SCENARIO");
        puts("{\"quotaReaderTest\":\"passed\",\"scenarios\":4,\"appServer\":\"fake-only\"}");
    }
    return 0;
}
