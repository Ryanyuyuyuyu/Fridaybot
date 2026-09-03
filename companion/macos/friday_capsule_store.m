/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#import "friday_capsule_store.h"

#import <AVFoundation/AVFoundation.h>
#import <Speech/Speech.h>

#include "../../main/apps/app_friday/capsule/capsule_codec.h"
#include "../../main/apps/app_friday/capsule/capsule_protocol.h"

static const NSTimeInterval FridayCapsuleAudioRetentionSeconds = 24.0 * 60.0 * 60.0;
static const NSTimeInterval FridayCapsuleTranscriptAckTimeoutSeconds = 6.0;
static const float FridayCapsuleMinimumReportedConfidence = 0.35f;

static NSString *FridayCapsuleDateKey(NSDate *date)
{
    static NSDateFormatter *formatter;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        formatter = [[NSDateFormatter alloc] init];
        formatter.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
        formatter.dateFormat = @"yyyy-MM-dd";
    });
    return [formatter stringFromDate:date];
}

static NSString *FridayCapsuleTimestamp(NSDate *date)
{
    static NSDateFormatter *formatter;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        formatter = [[NSDateFormatter alloc] init];
        formatter.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
        formatter.dateFormat = @"yyyyMMdd-HHmmss-SSS";
    });
    return [formatter stringFromDate:date];
}

static void AppendLE16(NSMutableData *data, uint16_t value)
{
    uint8_t bytes[2] = { (uint8_t)value, (uint8_t)(value >> 8U) };
    [data appendBytes:bytes length:sizeof(bytes)];
}

static void AppendLE32(NSMutableData *data, uint32_t value)
{
    uint8_t bytes[4] = {
        (uint8_t)value,
        (uint8_t)(value >> 8U),
        (uint8_t)(value >> 16U),
        (uint8_t)(value >> 24U),
    };
    [data appendBytes:bytes length:sizeof(bytes)];
}

static NSData *FridayCapsuleWavHeader(uint32_t samples)
{
    const uint32_t dataBytes = samples * sizeof(int16_t);
    NSMutableData *header = [NSMutableData dataWithCapacity:44];
    [header appendBytes:"RIFF" length:4];
    AppendLE32(header, 36U + dataBytes);
    [header appendBytes:"WAVEfmt " length:8];
    AppendLE32(header, 16);
    AppendLE16(header, 1);
    AppendLE16(header, 1);
    AppendLE32(header, FRIDAY_CAPSULE_SAMPLE_RATE);
    AppendLE32(header, FRIDAY_CAPSULE_SAMPLE_RATE * sizeof(int16_t));
    AppendLE16(header, sizeof(int16_t));
    AppendLE16(header, 16);
    [header appendBytes:"data" length:4];
    AppendLE32(header, dataBytes);
    return header;
}

static NSString *FridayCapsuleCategory(NSString *transcript)
{
    NSString *lower = transcript.lowercaseString;
    NSArray<NSString *> *taskMarkers = @[
        @"记得", @"提醒", @"别忘", @"需要", @"要去", @"要给", @"待办", @"完成", @"提交",
        @"购买", @"买", @"预约", @"联系", @"打电话", @"发送", @"回复",
        @"remember to", @"need to", @"todo", @"call ", @"buy ", @"send ", @"reply ",
    ];
    for (NSString *marker in taskMarkers) {
        if ([lower containsString:marker]) {
            return @"待办";
        }
    }
    return @"备忘";
}

static BOOL FridayCapsuleTranscriptionIsConfident(SFTranscription *transcription,
                                                   float *averageConfidence)
{
    NSString *text = [transcription.formattedString
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (text.length < 2) {
        if (averageConfidence != NULL) {
            *averageConfidence = 0.0f;
        }
        return NO;
    }

    double weightedTotal = 0.0;
    double totalWeight = 0.0;
    for (SFTranscriptionSegment *segment in transcription.segments) {
        // Some on-device recognizers do not report confidence and leave every
        // segment at zero. Treat that as unavailable instead of rejecting an
        // otherwise usable final transcript.
        if (segment.confidence <= 0.0f) {
            continue;
        }
        const double weight = MAX(0.1, segment.duration);
        weightedTotal += segment.confidence * weight;
        totalWeight += weight;
    }
    if (totalWeight == 0.0) {
        if (averageConfidence != NULL) {
            *averageConfidence = -1.0f;
        }
        return YES;
    }

    const float average = (float)(weightedTotal / totalWeight);
    if (averageConfidence != NULL) {
        *averageConfidence = average;
    }
    return average >= FridayCapsuleMinimumReportedConfidence;
}

@interface FridayCapsuleStore ()
- (void)prepareSpeechRecognition;
- (void)resumePendingTranscriptions;
- (void)startStreamingTranscriptionForItemID:(NSString *)itemID;
- (void)appendPCMToStreamingTranscription:(const int16_t *)pcm;
- (BOOL)finishStreamingTranscriptionForItemID:(NSString *)itemID;
- (void)cancelStreamingTranscription;
- (void)transcribeURL:(NSURL *)url itemID:(NSString *)itemID;
- (void)finishTranscriptionForItemID:(NSString *)itemID
                       transcription:(SFTranscription *)transcription
                               error:(NSError *)error;
- (void)finishPendingAcknowledgementForItemID:(NSString *)itemID
                                       status:(FridayCapsuleAckStatus)status
                                       detail:(FridayCapsuleAckDetail)detail;
- (void)purgeExpiredRetainedAudio;
- (void)loadToday;
- (BOOL)writeIndex:(NSError **)error;
- (NSMutableDictionary *)itemWithID:(NSString *)itemID;
- (void)discardCapture;
- (void)resetCapture;
@end

@implementation FridayCapsuleStore {
    NSFileManager *_files;
    NSMutableArray<NSMutableDictionary *> *_items;
    NSString *_loadedDateKey;
    NSMutableDictionary<NSString *, SFSpeechRecognitionTask *> *_speechTasks;
    SFSpeechRecognizer *_streamingRecognizer;
    SFSpeechAudioBufferRecognitionRequest *_streamingRequest;
    SFSpeechRecognitionTask *_streamingTask;
    NSString *_streamingItemID;
    NSFileHandle *_captureHandle;
    NSURL *_temporaryURL;
    NSDate *_captureStartedAt;
    NSDate *_lastPacketAt;
    NSDate *_lastRetentionSweepAt;
    NSString *_captureItemID;
    NSString *_pendingAckItemID;
    uint16_t _pendingAckSession;
    uint16_t _session;
    uint16_t _expectedSequence;
    uint32_t _sampleCount;
    BOOL _missingFrame;
}

- (instancetype)init
{
    self = [super init];
    if (self != nil) {
        _files = NSFileManager.defaultManager;
        _speechTasks = [NSMutableDictionary dictionary];
        [self loadToday];
        [self purgeExpiredRetainedAudio];
        __weak FridayCapsuleStore *weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf prepareSpeechRecognition];
        });
    }
    return self;
}

- (NSURL *)rootURL
{
    NSURL *applicationSupport = [_files URLForDirectory:NSApplicationSupportDirectory
                                               inDomain:NSUserDomainMask
                                      appropriateForURL:nil
                                                 create:YES
                                                  error:nil];
    return [applicationSupport URLByAppendingPathComponent:@"Friday/Flash Capsules" isDirectory:YES];
}

- (NSURL *)todayURLCreating:(BOOL)create error:(NSError **)error
{
    NSURL *url = [[self rootURL] URLByAppendingPathComponent:FridayCapsuleDateKey([NSDate date]) isDirectory:YES];
    if (create && ![_files createDirectoryAtURL:url withIntermediateDirectories:YES attributes:nil error:error]) {
        return nil;
    }
    return url;
}

- (NSURL *)indexURLCreating:(BOOL)create error:(NSError **)error
{
    NSURL *today = [self todayURLCreating:create error:error];
    return today == nil ? nil : [today URLByAppendingPathComponent:@"index.json"];
}

- (void)loadToday
{
    _loadedDateKey = FridayCapsuleDateKey([NSDate date]);
    _items = [NSMutableArray array];
    NSURL *indexURL = [self indexURLCreating:NO error:nil];
    NSData *data = indexURL == nil ? nil : [NSData dataWithContentsOfURL:indexURL];
    if (data == nil) {
        return;
    }
    id object = [NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingMutableContainers error:nil];
    if ([object isKindOfClass:NSArray.class]) {
        for (id value in (NSArray *)object) {
            if ([value isKindOfClass:NSDictionary.class]) {
                [_items addObject:[value mutableCopy]];
            }
        }
    }
}

- (NSArray<NSDictionary *> *)itemsForToday
{
    if (_items == nil || ![_loadedDateKey isEqualToString:FridayCapsuleDateKey([NSDate date])]) {
        [self loadToday];
    }
    return [_items copy];
}

- (NSURL *)audioURLForItem:(NSDictionary *)item
{
    NSString *fileName = item[@"audioFile"];
    if (![fileName isKindOfClass:NSString.class] || fileName.length == 0) {
        return nil;
    }
    NSURL *today = [self todayURLCreating:NO error:nil];
    return today == nil ? nil : [today URLByAppendingPathComponent:fileName];
}

- (void)toggleCompletedForItemID:(NSString *)itemID
{
    NSMutableDictionary *item = [self itemWithID:itemID];
    if (item == nil || ![item[@"category"] isEqualToString:@"待办"]) {
        return;
    }
    item[@"completed"] = @(![item[@"completed"] boolValue]);
    [self writeIndex:nil];
    if (self.onItemsChanged != nil) {
        self.onItemsChanged();
    }
}

- (BOOL)writeIndex:(NSError **)error
{
    NSURL *indexURL = [self indexURLCreating:YES error:error];
    if (indexURL == nil) {
        return NO;
    }
    NSData *json = [NSJSONSerialization dataWithJSONObject:_items options:NSJSONWritingPrettyPrinted error:error];
    return json != nil && [json writeToURL:indexURL options:NSDataWritingAtomic error:error];
}

- (void)publishFeedback:(FridayCapsuleFeedbackState)state progress:(CGFloat)progress
{
    if (self.onFeedback != nil) {
        self.onFeedback(state, progress);
    }
}

- (void)sendAckForSession:(uint16_t)session
                   status:(FridayCapsuleAckStatus)status
                   detail:(FridayCapsuleAckDetail)detail
{
    uint8_t bytes[FRIDAY_CAPSULE_ACK_PACKET_SIZE] = {};
    const size_t length = friday_capsule_encode_ack(bytes, session, status, detail);
    if (length != 0 && self.sendAcknowledgement != nil) {
        self.sendAcknowledgement([NSData dataWithBytes:bytes length:length]);
    }
}

- (void)handlePacket:(NSData *)data
{
    if (data.length < 4) {
        return;
    }
    const uint8_t *bytes = data.bytes;
    const uint16_t session = friday_capsule_read_u16(bytes + 2);
    if (bytes[0] != FRIDAY_CAPSULE_PROTOCOL_VERSION || session == 0) {
        [self sendAckForSession:session status:FridayCapsuleAckError detail:FridayCapsuleAckDetailProtocol];
        return;
    }

    if (bytes[1] == FridayCapsulePacketStart) {
        [self beginCapture:data session:session];
    } else if (bytes[1] == FridayCapsulePacketAudio) {
        [self appendAudio:data session:session];
    } else if (bytes[1] == FridayCapsulePacketEnd) {
        [self finishCapture:data session:session];
    } else {
        [self sendAckForSession:session status:FridayCapsuleAckError detail:FridayCapsuleAckDetailProtocol];
    }
}

- (void)beginCapture:(NSData *)data session:(uint16_t)session
{
    if (data.length != FRIDAY_CAPSULE_START_PACKET_SIZE) {
        [self sendAckForSession:session status:FridayCapsuleAckError detail:FridayCapsuleAckDetailProtocol];
        return;
    }
    const uint8_t *bytes = data.bytes;
    if (friday_capsule_read_u16(bytes + 4) != FRIDAY_CAPSULE_SAMPLE_RATE ||
        friday_capsule_read_u16(bytes + 6) != FRIDAY_CAPSULE_FRAME_SAMPLES ||
        bytes[8] != FridayCapsuleCodecImaAdpcm || bytes[9] != 60) {
        [self sendAckForSession:session status:FridayCapsuleAckError detail:FridayCapsuleAckDetailProtocol];
        return;
    }

    [self abortActiveCapture];
    if (![_loadedDateKey isEqualToString:FridayCapsuleDateKey([NSDate date])]) {
        [self loadToday];
    }
    NSError *error = nil;
    NSURL *today = [self todayURLCreating:YES error:&error];
    if (today == nil) {
        printf("[Capsule] Cannot create storage: %s\n", error.localizedDescription.UTF8String);
        [self sendAckForSession:session status:FridayCapsuleAckError detail:FridayCapsuleAckDetailStorage];
        [self publishFeedback:FridayCapsuleFeedbackFailed progress:1.0];
        return;
    }
    NSString *name = [NSString stringWithFormat:@"%@-%04x.wav.part", FridayCapsuleTimestamp([NSDate date]), session];
    _temporaryURL = [today URLByAppendingPathComponent:name];
    if (![_files createFileAtPath:_temporaryURL.path contents:FridayCapsuleWavHeader(0) attributes:nil]) {
        [self sendAckForSession:session status:FridayCapsuleAckError detail:FridayCapsuleAckDetailStorage];
        [self publishFeedback:FridayCapsuleFeedbackFailed progress:1.0];
        _temporaryURL = nil;
        return;
    }
    _captureHandle = [NSFileHandle fileHandleForWritingToURL:_temporaryURL error:&error];
    if (_captureHandle == nil) {
        [_files removeItemAtURL:_temporaryURL error:nil];
        _temporaryURL = nil;
        [self sendAckForSession:session status:FridayCapsuleAckError detail:FridayCapsuleAckDetailStorage];
        [self publishFeedback:FridayCapsuleFeedbackFailed progress:1.0];
        return;
    }
    [_captureHandle seekToEndOfFile];
    _session = session;
    _expectedSequence = 0;
    _sampleCount = 0;
    _missingFrame = NO;
    _captureStartedAt = [NSDate date];
    _lastPacketAt = _captureStartedAt;
    _captureItemID = NSUUID.UUID.UUIDString;
    [self startStreamingTranscriptionForItemID:_captureItemID];
    [self publishFeedback:FridayCapsuleFeedbackRecording progress:0.0];
    printf("[Capsule] Streaming session %u to temporary audio and live transcription\n", session);
}

- (void)appendAudio:(NSData *)data session:(uint16_t)session
{
    if (_captureHandle == nil || session != _session || data.length != FRIDAY_CAPSULE_AUDIO_PACKET_SIZE) {
        return;
    }
    const uint8_t *bytes = data.bytes;
    _lastPacketAt = [NSDate date];
    const uint16_t sequence = friday_capsule_read_u16(bytes + 4);
    if (sequence != _expectedSequence) {
        _missingFrame = YES;
        _expectedSequence = sequence;
    }
    ++_expectedSequence;
    if (bytes[8] > 88 || bytes[9] != FRIDAY_CAPSULE_FRAME_SAMPLES) {
        _missingFrame = YES;
        return;
    }

    int16_t pcm[FRIDAY_CAPSULE_FRAME_SAMPLES] = {};
    friday_capsule_adpcm_decode(bytes + FRIDAY_CAPSULE_AUDIO_HEADER_SIZE, pcm,
                                (int16_t)friday_capsule_read_u16(bytes + 6), bytes[8]);
    @try {
        [_captureHandle writeData:[NSData dataWithBytes:pcm length:sizeof(pcm)]];
    } @catch (NSException *exception) {
        (void)exception;
        _missingFrame = YES;
        return;
    }
    [self appendPCMToStreamingTranscription:pcm];
    _sampleCount += FRIDAY_CAPSULE_FRAME_SAMPLES;
    if ((_expectedSequence % 5U) == 0U) {
        const CGFloat progress = MIN(1.0, (CGFloat)_sampleCount /
                                            (FRIDAY_CAPSULE_SAMPLE_RATE * 60.0));
        [self publishFeedback:FridayCapsuleFeedbackRecording progress:progress];
    }
}

- (void)finishCapture:(NSData *)data session:(uint16_t)session
{
    if (data.length != FRIDAY_CAPSULE_END_PACKET_SIZE || _captureHandle == nil || session != _session) {
        [self sendAckForSession:session status:FridayCapsuleAckError detail:FridayCapsuleAckDetailProtocol];
        return;
    }
    const uint8_t *bytes = data.bytes;
    const FridayCapsuleEndReason reason = (FridayCapsuleEndReason)bytes[4];
    const uint32_t declaredSamples = friday_capsule_read_u32(bytes + 6);
    if (declaredSamples != _sampleCount) {
        _missingFrame = YES;
    }

    if (reason == FridayCapsuleEndTooShort || reason == FridayCapsuleEndCanceled) {
        [self discardCapture];
        [self sendAckForSession:session status:FridayCapsuleAckDiscarded detail:FridayCapsuleAckDetailTooShort];
        [self publishFeedback:FridayCapsuleFeedbackIdle progress:0.0];
        return;
    }
    if (_missingFrame || reason == FridayCapsuleEndTransportError) {
        [self discardCapture];
        [self sendAckForSession:session status:FridayCapsuleAckError detail:FridayCapsuleAckDetailMissingFrame];
        [self publishFeedback:FridayCapsuleFeedbackFailed progress:1.0];
        return;
    }
    if (_sampleCount < FRIDAY_CAPSULE_SAMPLE_RATE * 8U / 10U) {
        [self discardCapture];
        [self sendAckForSession:session status:FridayCapsuleAckDiscarded detail:FridayCapsuleAckDetailTooShort];
        [self publishFeedback:FridayCapsuleFeedbackIdle progress:0.0];
        return;
    }

    [self publishFeedback:FridayCapsuleFeedbackAwaitingSave progress:1.0];
    NSError *error = nil;
    @try {
        [_captureHandle seekToFileOffset:0];
        [_captureHandle writeData:FridayCapsuleWavHeader(_sampleCount)];
        [_captureHandle synchronizeFile];
        [_captureHandle closeFile];
    } @catch (NSException *exception) {
        error = [NSError errorWithDomain:@"FridayCapsule" code:1
                                userInfo:@{NSLocalizedDescriptionKey : exception.reason ?: @"WAV write failed"}];
    }
    _captureHandle = nil;

    NSURL *finalURL = [_temporaryURL URLByDeletingPathExtension];
    if (error == nil && ![_files moveItemAtURL:_temporaryURL toURL:finalURL error:&error]) {
        finalURL = nil;
    }
    if (error != nil || finalURL == nil) {
        [_files removeItemAtURL:_temporaryURL error:nil];
        [self resetCapture];
        [self sendAckForSession:session status:FridayCapsuleAckError detail:FridayCapsuleAckDetailStorage];
        [self publishFeedback:FridayCapsuleFeedbackFailed progress:1.0];
        return;
    }

    NSString *itemID = _captureItemID ?: NSUUID.UUID.UUIDString;
    NSMutableDictionary *item = [@{
        @"id" : itemID,
        @"createdAt" : @([_captureStartedAt timeIntervalSince1970]),
        @"duration" : @((double)_sampleCount / FRIDAY_CAPSULE_SAMPLE_RATE),
        @"audioFile" : finalURL.lastPathComponent,
        @"transcript" : @"",
        @"category" : @"待确认",
        @"transcriptionState" : @"pending",
        @"completed" : @NO,
    } mutableCopy];
    [_items insertObject:item atIndex:0];
    if (![self writeIndex:&error]) {
        [_items removeObject:item];
        [_files removeItemAtURL:finalURL error:nil];
        [self cancelStreamingTranscription];
        [self resetCapture];
        [self sendAckForSession:session status:FridayCapsuleAckError detail:FridayCapsuleAckDetailStorage];
        [self publishFeedback:FridayCapsuleFeedbackFailed progress:1.0];
        return;
    }

    _pendingAckItemID = itemID;
    _pendingAckSession = session;
    const BOOL liveTranscriptionFinishing = [self finishStreamingTranscriptionForItemID:itemID];
    [self resetCapture];
    if (self.onItemsChanged != nil) {
        self.onItemsChanged();
    }
    if (!liveTranscriptionFinishing) {
        [self transcribeURL:finalURL itemID:itemID];
    }
    __weak FridayCapsuleStore *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                 (int64_t)(FridayCapsuleTranscriptAckTimeoutSeconds * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        FridayCapsuleStore *strongSelf = weakSelf;
        if (strongSelf == nil || ![strongSelf->_pendingAckItemID isEqualToString:itemID]) {
            return;
        }
        [strongSelf cancelStreamingTranscription];
        [strongSelf->_speechTasks[itemID] cancel];
        [strongSelf->_speechTasks removeObjectForKey:itemID];
        NSError *timeout = [NSError errorWithDomain:@"FridayCapsule" code:5
                                           userInfo:@{NSLocalizedDescriptionKey : @"Transcript timed out"}];
        [strongSelf finishTranscriptionForItemID:itemID transcription:nil error:timeout];
    });
    printf("[Capsule] Captured %s; awaiting final transcript before acknowledgement\n",
           finalURL.path.UTF8String);
}

- (void)discardCapture
{
    [self cancelStreamingTranscription];
    @try {
        [_captureHandle closeFile];
    } @catch (NSException *exception) {
        (void)exception;
    }
    [_files removeItemAtURL:_temporaryURL error:nil];
    [self resetCapture];
}

- (void)resetCapture
{
    _captureHandle = nil;
    _temporaryURL = nil;
    _captureStartedAt = nil;
    _lastPacketAt = nil;
    _captureItemID = nil;
    _session = 0;
    _expectedSequence = 0;
    _sampleCount = 0;
    _missingFrame = NO;
}

- (void)abortActiveCapture
{
    if (_captureHandle != nil || _temporaryURL != nil) {
        [self discardCapture];
        [self publishFeedback:FridayCapsuleFeedbackFailed progress:1.0];
    }
}

- (void)expireStaleCapture
{
    if (_lastRetentionSweepAt == nil || -[_lastRetentionSweepAt timeIntervalSinceNow] >= 60.0) {
        [self purgeExpiredRetainedAudio];
    }
    if (_captureHandle == nil || _lastPacketAt == nil || -[_lastPacketAt timeIntervalSinceNow] <= 3.0) {
        return;
    }
    const uint16_t session = _session;
    [self discardCapture];
    [self sendAckForSession:session status:FridayCapsuleAckError detail:FridayCapsuleAckDetailMissingFrame];
    [self publishFeedback:FridayCapsuleFeedbackFailed progress:1.0];
    printf("[Capsule] Discarded stalled session %u\n", session);
}

- (NSMutableDictionary *)itemWithID:(NSString *)itemID
{
    for (NSMutableDictionary *item in _items) {
        if ([item[@"id"] isEqualToString:itemID]) {
            return item;
        }
    }
    return nil;
}

- (void)prepareSpeechRecognition
{
    const SFSpeechRecognizerAuthorizationStatus status = SFSpeechRecognizer.authorizationStatus;
    if (status == SFSpeechRecognizerAuthorizationStatusNotDetermined) {
        __weak FridayCapsuleStore *weakSelf = self;
        [SFSpeechRecognizer requestAuthorization:^(SFSpeechRecognizerAuthorizationStatus nextStatus) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (nextStatus == SFSpeechRecognizerAuthorizationStatusAuthorized) {
                    [weakSelf resumePendingTranscriptions];
                }
            });
        }];
    } else if (status == SFSpeechRecognizerAuthorizationStatusAuthorized) {
        [self resumePendingTranscriptions];
    }
}

- (void)resumePendingTranscriptions
{
    for (NSDictionary *item in [self.itemsForToday copy]) {
        NSString *state = item[@"transcriptionState"];
        NSString *itemID = item[@"id"];
        NSURL *url = [self audioURLForItem:item];
        if (([state isEqualToString:@"pending"] || [state isEqualToString:@"finalizing"]) &&
            itemID.length > 0 && url != nil && [_files fileExistsAtPath:url.path] &&
            _speechTasks[itemID] == nil) {
            [self transcribeURL:url itemID:itemID];
        }
    }
}

- (SFSpeechRecognizer *)newSpeechRecognizer
{
    SFSpeechRecognizer *recognizer = [[SFSpeechRecognizer alloc]
        initWithLocale:[NSLocale localeWithLocaleIdentifier:@"zh-CN"]];
    return recognizer.available ? recognizer : nil;
}

- (void)configureSpeechRequest:(SFSpeechRecognitionRequest *)request
{
    request.addsPunctuation = YES;
    request.taskHint = SFSpeechRecognitionTaskHintDictation;
    request.contextualStrings = @[
        @"闪念胶囊", @"待办", @"备忘", @"提醒", @"记得", @"别忘了", @"打电话", @"发消息",
        @"Friday", @"StopWatch", @"Codex",
    ];
}

- (void)startStreamingTranscriptionForItemID:(NSString *)itemID
{
    [self cancelStreamingTranscription];
    if (SFSpeechRecognizer.authorizationStatus != SFSpeechRecognizerAuthorizationStatusAuthorized) {
        return;
    }

    SFSpeechRecognizer *recognizer = [self newSpeechRecognizer];
    if (recognizer == nil) {
        return;
    }
    SFSpeechAudioBufferRecognitionRequest *request =
        [[SFSpeechAudioBufferRecognitionRequest alloc] init];
    request.shouldReportPartialResults = YES;
    [self configureSpeechRequest:request];

    _streamingRecognizer = recognizer;
    _streamingRequest = request;
    _streamingItemID = [itemID copy];
    __weak FridayCapsuleStore *weakSelf = self;
    _streamingTask = [recognizer recognitionTaskWithRequest:request
        resultHandler:^(SFSpeechRecognitionResult *result, NSError *error) {
            if (!result.final && error == nil) {
                return;
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                FridayCapsuleStore *strongSelf = weakSelf;
                if (strongSelf == nil) {
                    return;
                }
                if (result.final) {
                    [strongSelf finishTranscriptionForItemID:itemID
                                               transcription:result.bestTranscription
                                                       error:nil];
                } else {
                    [strongSelf finishTranscriptionForItemID:itemID transcription:nil error:error];
                }
            });
        }];
    if (_streamingTask != nil) {
        printf("[Capsule] Live transcription started\n");
    } else {
        _streamingRecognizer = nil;
        _streamingRequest = nil;
        _streamingItemID = nil;
    }
}

- (void)appendPCMToStreamingTranscription:(const int16_t *)pcm
{
    if (_streamingRequest == nil || pcm == NULL) {
        return;
    }
    AVAudioFormat *format = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatInt16
                                                             sampleRate:FRIDAY_CAPSULE_SAMPLE_RATE
                                                               channels:1
                                                            interleaved:NO];
    AVAudioPCMBuffer *buffer = [[AVAudioPCMBuffer alloc]
        initWithPCMFormat:format frameCapacity:FRIDAY_CAPSULE_FRAME_SAMPLES];
    if (buffer == nil || buffer.int16ChannelData == NULL) {
        return;
    }
    buffer.frameLength = FRIDAY_CAPSULE_FRAME_SAMPLES;
    memcpy(buffer.int16ChannelData[0], pcm, FRIDAY_CAPSULE_FRAME_SAMPLES * sizeof(int16_t));
    [_streamingRequest appendAudioPCMBuffer:buffer];
}

- (BOOL)finishStreamingTranscriptionForItemID:(NSString *)itemID
{
    if (_streamingRequest == nil || _streamingTask == nil ||
        ![_streamingItemID isEqualToString:itemID]) {
        return NO;
    }
    [_streamingRequest endAudio];
    return YES;
}

- (void)cancelStreamingTranscription
{
    [_streamingRequest endAudio];
    [_streamingTask cancel];
    _streamingRecognizer = nil;
    _streamingRequest = nil;
    _streamingTask = nil;
    _streamingItemID = nil;
}

- (void)finishPendingAcknowledgementForItemID:(NSString *)itemID
                                       status:(FridayCapsuleAckStatus)status
                                       detail:(FridayCapsuleAckDetail)detail
{
    if (![_pendingAckItemID isEqualToString:itemID] || _pendingAckSession == 0) {
        return;
    }
    [self sendAckForSession:_pendingAckSession status:status detail:detail];
    [self publishFeedback:(status == FridayCapsuleAckSaved ? FridayCapsuleFeedbackSaved
                                                           : FridayCapsuleFeedbackFailed)
                  progress:1.0];
    _pendingAckItemID = nil;
    _pendingAckSession = 0;
}

- (void)finishTranscriptionForItemID:(NSString *)itemID
                       transcription:(SFTranscription *)transcription
                               error:(NSError *)error
{
    NSMutableDictionary *item = [self itemWithID:itemID];
    if (item == nil) {
        return;
    }

    NSString *text = [transcription.formattedString
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    float confidence = 0.0f;
    const BOOL confident = transcription != nil &&
        FridayCapsuleTranscriptionIsConfident(transcription, &confidence);
    NSMutableDictionary *previous = [item mutableCopy];
    NSURL *audioURL = [self audioURLForItem:item];
    NSString *audioFile = item[@"audioFile"];

    if (text.length > 0 && confident) {
        item[@"transcript"] = text;
        item[@"category"] = FridayCapsuleCategory(text);
        item[@"transcriptionState"] = @"complete";
        if (confidence >= 0.0f) {
            item[@"transcriptionConfidence"] = @(confidence);
        } else {
            [item removeObjectForKey:@"transcriptionConfidence"];
        }
        [item removeObjectForKey:@"transcriptionError"];
        [item removeObjectForKey:@"audioRetainUntil"];
        [item removeObjectForKey:@"audioFile"];

        NSError *indexError = nil;
        if (![self writeIndex:&indexError]) {
            [item setDictionary:previous];
            [self finishPendingAcknowledgementForItemID:itemID
                                                  status:FridayCapsuleAckError
                                                  detail:FridayCapsuleAckDetailStorage];
            return;
        }

        NSError *removeError = nil;
        if (audioURL != nil && [_files fileExistsAtPath:audioURL.path] &&
            ![_files removeItemAtURL:audioURL error:&removeError]) {
            if (audioFile.length > 0) {
                item[@"audioFile"] = audioFile;
            }
            item[@"audioRetainUntil"] = @([[NSDate date]
                dateByAddingTimeInterval:FridayCapsuleAudioRetentionSeconds].timeIntervalSince1970);
            item[@"audioCleanupError"] = removeError.localizedDescription ?: @"Audio cleanup failed";
            [self writeIndex:nil];
            [self finishPendingAcknowledgementForItemID:itemID
                                                  status:FridayCapsuleAckError
                                                  detail:FridayCapsuleAckDetailStorage];
        } else {
            [item removeObjectForKey:@"audioCleanupError"];
            [self finishPendingAcknowledgementForItemID:itemID
                                                  status:FridayCapsuleAckSaved
                                                  detail:FridayCapsuleAckDetailNone];
            printf("[Capsule] Transcript saved and temporary audio deleted: %s\n", text.UTF8String);
        }
    } else {
        if (text.length > 0) {
            item[@"transcript"] = text;
            item[@"transcriptionState"] = @"needsReview";
            if (confidence >= 0.0f) {
                item[@"transcriptionConfidence"] = @(confidence);
            }
        } else {
            item[@"transcriptionState"] = @"error";
        }
        item[@"category"] = @"待确认";
        item[@"audioRetainUntil"] = @([[NSDate date]
            dateByAddingTimeInterval:FridayCapsuleAudioRetentionSeconds].timeIntervalSince1970);
        item[@"transcriptionError"] = error.localizedDescription ?: @"Low transcription confidence";
        NSError *indexError = nil;
        const BOOL written = [self writeIndex:&indexError];
        if (!written) {
            [item setDictionary:previous];
        }
        [self finishPendingAcknowledgementForItemID:itemID
                                              status:FridayCapsuleAckError
                                              detail:(written ? FridayCapsuleAckDetailTranscription
                                                              : FridayCapsuleAckDetailStorage)];
        printf("[Capsule] Transcript needs review; temporary audio retained for up to 24 hours\n");
    }

    if ([_streamingItemID isEqualToString:itemID]) {
        _streamingRecognizer = nil;
        _streamingRequest = nil;
        _streamingTask = nil;
        _streamingItemID = nil;
    }
    [_speechTasks removeObjectForKey:itemID];
    if (self.onItemsChanged != nil) {
        self.onItemsChanged();
    }
}

- (void)transcribeURL:(NSURL *)url itemID:(NSString *)itemID
{
    SFSpeechRecognizerAuthorizationStatus status = SFSpeechRecognizer.authorizationStatus;
    if (status == SFSpeechRecognizerAuthorizationStatusNotDetermined) {
        __weak FridayCapsuleStore *weakSelf = self;
        [SFSpeechRecognizer requestAuthorization:^(SFSpeechRecognizerAuthorizationStatus nextStatus) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (nextStatus == SFSpeechRecognizerAuthorizationStatusAuthorized) {
                    [weakSelf transcribeURL:url itemID:itemID];
                } else {
                    NSError *error = [NSError errorWithDomain:@"FridayCapsule" code:2
                                                    userInfo:@{NSLocalizedDescriptionKey : @"Speech permission denied"}];
                    [weakSelf finishTranscriptionForItemID:itemID transcription:nil error:error];
                }
            });
        }];
        return;
    }
    if (status != SFSpeechRecognizerAuthorizationStatusAuthorized) {
        NSError *error = [NSError errorWithDomain:@"FridayCapsule" code:3
                                        userInfo:@{NSLocalizedDescriptionKey : @"Speech recognition unavailable"}];
        [self finishTranscriptionForItemID:itemID transcription:nil error:error];
        return;
    }

    SFSpeechRecognizer *recognizer = [self newSpeechRecognizer];
    if (recognizer == nil) {
        NSError *error = [NSError errorWithDomain:@"FridayCapsule" code:4
                                        userInfo:@{NSLocalizedDescriptionKey : @"Speech recognizer unavailable"}];
        [self finishTranscriptionForItemID:itemID transcription:nil error:error];
        return;
    }
    SFSpeechURLRecognitionRequest *request = [[SFSpeechURLRecognitionRequest alloc] initWithURL:url];
    request.shouldReportPartialResults = NO;
    [self configureSpeechRequest:request];

    __weak FridayCapsuleStore *weakSelf = self;
    SFSpeechRecognitionTask *task = [recognizer recognitionTaskWithRequest:request
        resultHandler:^(SFSpeechRecognitionResult *result, NSError *error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                FridayCapsuleStore *strongSelf = weakSelf;
                if (strongSelf == nil) {
                    return;
                }
                if (result.final) {
                    [strongSelf finishTranscriptionForItemID:itemID
                                               transcription:result.bestTranscription
                                                       error:nil];
                    [strongSelf->_speechTasks removeObjectForKey:itemID];
                } else if (error != nil) {
                    [strongSelf finishTranscriptionForItemID:itemID transcription:nil error:error];
                    [strongSelf->_speechTasks removeObjectForKey:itemID];
                }
            });
        }];
    if (task != nil) {
        _speechTasks[itemID] = task;
    } else {
        NSError *error = [NSError errorWithDomain:@"FridayCapsule" code:6
                                        userInfo:@{NSLocalizedDescriptionKey : @"Speech task could not start"}];
        [self finishTranscriptionForItemID:itemID transcription:nil error:error];
    }
}

- (void)purgeExpiredRetainedAudio
{
    _lastRetentionSweepAt = [NSDate date];
    const NSTimeInterval now = _lastRetentionSweepAt.timeIntervalSince1970;
    BOOL changed = NO;
    for (NSMutableDictionary *item in _items) {
        NSString *audioFile = item[@"audioFile"];
        if (audioFile.length == 0) {
            continue;
        }
        const BOOL completed = [item[@"transcriptionState"] isEqualToString:@"complete"];
        NSNumber *retainUntil = item[@"audioRetainUntil"];
        const BOOL expired = retainUntil != nil && retainUntil.doubleValue <= now;
        if (!completed && !expired) {
            continue;
        }

        NSURL *url = [self audioURLForItem:item];
        NSError *error = nil;
        if (url != nil && [_files fileExistsAtPath:url.path] && ![_files removeItemAtURL:url error:&error]) {
            printf("[Capsule] Could not delete retained audio: %s\n",
                   error.localizedDescription.UTF8String ?: "unknown error");
            continue;
        }
        [item removeObjectForKey:@"audioFile"];
        [item removeObjectForKey:@"audioRetainUntil"];
        [item removeObjectForKey:@"audioCleanupError"];
        if (expired && !completed) {
            item[@"transcriptionState"] = @"expired";
            item[@"transcriptionError"] = @"Temporary audio expired; record again to retry";
        }
        changed = YES;
    }
    if (changed) {
        [self writeIndex:nil];
        if (self.onItemsChanged != nil) {
            self.onItemsChanged();
        }
    }
}

@end
