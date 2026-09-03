/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#import "friday_capsule_inbox.h"

#import "friday_capsule_store.h"
#import <AVFoundation/AVFoundation.h>

@interface FridayCapsuleRowView : NSTableCellView
@property(nonatomic, copy) void (^onPlay)(NSString *itemID);
@property(nonatomic, copy) void (^onToggle)(NSString *itemID);
- (void)showItem:(NSDictionary *)item;
@end

@implementation FridayCapsuleRowView {
    NSButton *_completion;
    NSTextField *_transcript;
    NSTextField *_metadata;
    NSButton *_play;
    NSString *_itemID;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self != nil) {
        _completion = [NSButton checkboxWithTitle:@"" target:self action:@selector(toggle:)];
        _completion.frame = NSMakeRect(10, 38, 22, 22);
        [self addSubview:_completion];

        _transcript = [NSTextField wrappingLabelWithString:@""];
        _transcript.frame = NSMakeRect(42, 28, 300, 38);
        _transcript.maximumNumberOfLines = 2;
        _transcript.lineBreakMode = NSLineBreakByTruncatingTail;
        _transcript.font = [NSFont systemFontOfSize:13.5 weight:NSFontWeightMedium];
        [self addSubview:_transcript];

        _metadata = [NSTextField labelWithString:@""];
        _metadata.frame = NSMakeRect(42, 7, 300, 18);
        _metadata.font = [NSFont systemFontOfSize:11.0];
        _metadata.textColor = NSColor.secondaryLabelColor;
        [self addSubview:_metadata];

        _play = [NSButton buttonWithTitle:@"▶︎" target:self action:@selector(play:)];
        _play.bezelStyle = NSBezelStyleCircular;
        _play.frame = NSMakeRect(350, 25, 34, 34);
        _play.toolTip = @"播放原音";
        [self addSubview:_play];
    }
    return self;
}

- (void)showItem:(NSDictionary *)item
{
    _itemID = item[@"id"];
    NSString *category = item[@"category"] ?: @"待确认";
    NSString *state = item[@"transcriptionState"];
    NSString *transcript = item[@"transcript"];
    if (transcript.length == 0) {
        if ([state isEqualToString:@"error"] || [state isEqualToString:@"needsReview"]) {
            transcript = item[@"audioFile"] != nil ? @"转写待确认，可播放临时原音" : @"转写未完成，请重新录制";
        } else if ([state isEqualToString:@"expired"]) {
            transcript = @"临时原音已到期，请重新录制";
        } else {
            transcript = @"正在转写…";
        }
    }
    _transcript.stringValue = transcript;
    const BOOL needsReview = [state isEqualToString:@"error"] ||
        [state isEqualToString:@"needsReview"] || [state isEqualToString:@"expired"];
    _transcript.textColor = needsReview ? NSColor.secondaryLabelColor : NSColor.labelColor;

    NSTimeInterval timestamp = [item[@"createdAt"] doubleValue];
    NSDate *date = [NSDate dateWithTimeIntervalSince1970:timestamp];
    static NSDateFormatter *timeFormatter;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        timeFormatter = [[NSDateFormatter alloc] init];
        timeFormatter.dateFormat = @"HH:mm";
    });
    _metadata.stringValue = [NSString stringWithFormat:@"%@ · %@ · %.1f 秒", category,
                             [timeFormatter stringFromDate:date], [item[@"duration"] doubleValue]];

    BOOL isTask = [category isEqualToString:@"待办"];
    _completion.hidden = !isTask;
    _completion.state = [item[@"completed"] boolValue] ? NSControlStateValueOn : NSControlStateValueOff;
    _play.hidden = item[@"audioFile"] == nil;
}

- (void)toggle:(id)sender
{
    (void)sender;
    if (self.onToggle != nil && _itemID != nil) {
        self.onToggle(_itemID);
    }
}

- (void)play:(id)sender
{
    (void)sender;
    if (self.onPlay != nil && _itemID != nil) {
        self.onPlay(_itemID);
    }
}

@end

@interface FridayCapsuleInboxController ()
- (void)playItemID:(NSString *)itemID;
@end

@implementation FridayCapsuleInboxController {
    FridayCapsuleStore *_store;
    NSStatusItem *_statusItem;
    NSPopover *_popover;
    NSTableView *_table;
    NSTextField *_emptyLabel;
    NSArray<NSDictionary *> *_items;
    AVAudioPlayer *_player;
}

- (instancetype)initWithStore:(FridayCapsuleStore *)store
{
    self = [super init];
    if (self != nil) {
        _store = store;
        [self buildStatusItem];
        [self buildPopover];
        [self refresh];
    }
    return self;
}

- (void)buildStatusItem
{
    _statusItem = [NSStatusBar.systemStatusBar statusItemWithLength:NSSquareStatusItemLength];
    NSStatusBarButton *button = _statusItem.button;
    button.image = [NSImage imageWithSystemSymbolName:@"waveform.circle.fill"
                             accessibilityDescription:@"闪念胶囊"];
    button.toolTip = @"今日闪念胶囊";
    button.target = self;
    button.action = @selector(togglePopover:);
}

- (void)buildPopover
{
    NSViewController *controller = [[NSViewController alloc] init];
    NSView *root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 410, 500)];

    NSTextField *title = [NSTextField labelWithString:@"今日闪念胶囊"];
    title.frame = NSMakeRect(18, 458, 300, 28);
    title.font = [NSFont systemFontOfSize:20 weight:NSFontWeightSemibold];
    [root addSubview:title];

    NSTextField *subtitle = [NSTextField labelWithString:@"转写成功后自动删除原音 · 待确认原音最多保留 24 小时"];
    subtitle.frame = NSMakeRect(19, 437, 370, 18);
    subtitle.font = [NSFont systemFontOfSize:11.5];
    subtitle.textColor = NSColor.secondaryLabelColor;
    [root addSubview:subtitle];

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(10, 15, 390, 412)];
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    _table = [[NSTableView alloc] initWithFrame:scroll.bounds];
    NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"capsule"];
    column.width = 390;
    [_table addTableColumn:column];
    _table.headerView = nil;
    _table.rowHeight = 74;
    _table.intercellSpacing = NSMakeSize(0, 2);
    _table.backgroundColor = NSColor.clearColor;
    _table.dataSource = self;
    _table.delegate = self;
    _table.target = self;
    _table.doubleAction = @selector(playSelected:);
    scroll.documentView = _table;
    [root addSubview:scroll];

    _emptyLabel = [NSTextField wrappingLabelWithString:@"今天还没有闪念胶囊\n按住 StopWatch 的 A 键，说完后松开。"];
    _emptyLabel.frame = NSMakeRect(55, 215, 300, 54);
    _emptyLabel.alignment = NSTextAlignmentCenter;
    _emptyLabel.textColor = NSColor.secondaryLabelColor;
    [root addSubview:_emptyLabel];

    controller.view = root;
    _popover = [[NSPopover alloc] init];
    _popover.contentViewController = controller;
    _popover.contentSize = root.frame.size;
    _popover.behavior = NSPopoverBehaviorTransient;
}

- (void)togglePopover:(id)sender
{
    (void)sender;
    if (_popover.shown) {
        [_popover performClose:nil];
    } else {
        [self refresh];
        [_popover showRelativeToRect:_statusItem.button.bounds
                              ofView:_statusItem.button
                       preferredEdge:NSRectEdgeMinY];
    }
}

- (void)refresh
{
    _items = [_store itemsForToday];
    [_table reloadData];
    _emptyLabel.hidden = _items.count != 0;
    _table.hidden = _items.count == 0;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView
{
    (void)tableView;
    return (NSInteger)_items.count;
}

- (NSView *)tableView:(NSTableView *)tableView
   viewForTableColumn:(NSTableColumn *)tableColumn
                  row:(NSInteger)row
{
    (void)tableColumn;
    FridayCapsuleRowView *view = [tableView makeViewWithIdentifier:@"FridayCapsuleRow" owner:self];
    if (view == nil) {
        view = [[FridayCapsuleRowView alloc] initWithFrame:NSMakeRect(0, 0, 390, 74)];
        view.identifier = @"FridayCapsuleRow";
        __weak FridayCapsuleInboxController *weakSelf = self;
        view.onPlay = ^(NSString *itemID) {
            [weakSelf playItemID:itemID];
        };
        view.onToggle = ^(NSString *itemID) {
            FridayCapsuleInboxController *strongSelf = weakSelf;
            [strongSelf->_store toggleCompletedForItemID:itemID];
        };
    }
    [view showItem:_items[(NSUInteger)row]];
    return view;
}

- (void)playSelected:(id)sender
{
    (void)sender;
    NSInteger row = _table.clickedRow >= 0 ? _table.clickedRow : _table.selectedRow;
    if (row >= 0 && row < (NSInteger)_items.count) {
        [self playItemID:_items[(NSUInteger)row][@"id"]];
    }
}

- (void)playItemID:(NSString *)itemID
{
    for (NSDictionary *item in _items) {
        if (![item[@"id"] isEqualToString:itemID]) {
            continue;
        }
        NSURL *url = [_store audioURLForItem:item];
        if (url == nil) {
            return;
        }
        NSError *error = nil;
        _player = [[AVAudioPlayer alloc] initWithContentsOfURL:url error:&error];
        if (_player == nil) {
            NSBeep();
            return;
        }
        [_player play];
        return;
    }
}

@end
