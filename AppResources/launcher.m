#import <Cocoa/Cocoa.h>
#import <IOKit/hidsystem/IOHIDLib.h>

static void showAlert(NSString *title, NSString *message) {
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = title;
    alert.informativeText = message;
    [alert addButtonWithTitle:@"OK"];
    [NSApp activateIgnoringOtherApps:YES];
    [alert runModal];
}

@interface KoneXPDelegate : NSObject <NSApplicationDelegate>
@property NSWindow *window;
@property NSPopUpButton *profile, *layer, *button, *action, *key;
@property NSTextField *status;
@property NSString *readerPath;
@end

@implementation KoneXPDelegate

- (NSTextField *)label:(NSString *)text frame:(NSRect)frame {
    NSTextField *label = [[NSTextField alloc] initWithFrame:frame];
    label.stringValue = text; label.editable = NO; label.bezeled = NO;
    label.drawsBackground = NO;
    return label;
}

- (NSPopUpButton *)popup:(NSRect)frame labels:(NSArray<NSString *> *)labels {
    NSPopUpButton *popup = [[NSPopUpButton alloc] initWithFrame:frame pullsDown:NO];
    [popup addItemsWithTitles:labels];
    return popup;
}

- (void)addAction:(NSString *)label slug:(NSString *)slug {
    [self.action addItemWithTitle:label]; self.action.lastItem.representedObject = slug;
}

- (void)addKey:(NSString *)label usage:(NSInteger)usage {
    [self.key addItemWithTitle:label];
    self.key.lastItem.representedObject = [NSString stringWithFormat:@"%ld", usage];
}

- (NSDictionary *)runReader:(NSArray<NSString *> *)arguments {
    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:self.readerPath]; task.arguments = arguments;
    NSPipe *pipe = [NSPipe pipe]; task.standardOutput = pipe; task.standardError = pipe;
    NSError *error = nil;
    if (![task launchAndReturnError:&error])
        return @{ @"status": @(-1), @"output": error.localizedDescription ?: @"Could not start the device helper." };
    [task waitUntilExit];
    NSData *data = [pipe.fileHandleForReading readDataToEndOfFile];
    NSString *output = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
    return @{ @"status": @(task.terminationStatus), @"output": output };
}

- (NSString *)slugForHex:(NSString *)hex {
    NSDictionary *known = @{
        @"00000000": @"disabled", @"00000101": @"left", @"00000201": @"right",
        @"00000301": @"middle", @"00000501": @"forward", @"00000601": @"back",
        @"00000701": @"tilt-left", @"00000801": @"tilt-right",
        @"00000901": @"scroll-up", @"00000a01": @"scroll-down",
        @"00000202": @"dpi-up", @"00000302": @"dpi-down",
        @"00000403": @"play-pause", @"00000203": @"previous-track",
        @"00000303": @"next-track", @"00000703": @"volume-up",
        @"00000803": @"volume-down", @"00000108": @"profile-cycle",
        @"0000010a": @"easy-shift"
    };
    NSString *slug = known[hex.lowercaseString];
    if (slug) return slug;
    if (hex.length == 8 && [[hex substringWithRange:NSMakeRange(6, 2)] isEqualToString:@"06"] &&
        [[hex substringWithRange:NSMakeRange(0, 2)] isEqualToString:@"00"] &&
        [[hex substringWithRange:NSMakeRange(4, 2)] isEqualToString:@"00"]) return @"key";
    return nil;
}

- (void)selectActionSlug:(NSString *)slug {
    for (NSMenuItem *item in self.action.itemArray)
        if ([item.representedObject isEqual:slug]) { [self.action selectItem:item]; return; }
}

- (void)actionChanged:(id)sender {
    (void)sender;
    self.key.enabled = [self.action.selectedItem.representedObject isEqualToString:@"key"];
}

- (void)readCurrent:(id)sender {
    (void)sender; self.status.stringValue = @"Reading the mouse…";
    NSInteger profile = self.profile.indexOfSelectedItem + 1;
    NSString *layer = self.layer.indexOfSelectedItem == 0 ? @"standard" : @"shift";
    NSInteger button = self.button.indexOfSelectedItem + 1;
    NSDictionary *result = [self runReader:@[@"buttons", [NSString stringWithFormat:@"%ld", profile]]];
    if ([result[@"status"] intValue] != 0) { self.status.stringValue = result[@"output"]; return; }
    NSString *prefix = [NSString stringWithFormat:@"%ld\t%@\t", button, layer];
    for (NSString *line in [result[@"output"] componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
        if (![line hasPrefix:prefix]) continue;
        NSString *hex = [line substringFromIndex:prefix.length];
        NSString *slug = [self slugForHex:hex];
        if (!slug) {
            self.status.stringValue = [NSString stringWithFormat:@"Unsupported current assignment (%@). Choose a new action to replace it.", hex];
            return;
        }
        [self selectActionSlug:slug];
        if ([slug isEqualToString:@"key"] && hex.length == 8) {
            unsigned usage = 0;
            [[NSScanner scannerWithString:[hex substringWithRange:NSMakeRange(2, 2)]] scanHexInt:&usage];
            for (NSMenuItem *item in self.key.itemArray)
                if ([item.representedObject integerValue] == usage) [self.key selectItem:item];
        }
        [self actionChanged:nil];
        self.status.stringValue = [NSString stringWithFormat:@"Current assignment loaded from Profile %ld.", profile];
        return;
    }
    self.status.stringValue = @"The selected assignment was not returned by the mouse.";
}

- (void)identifyPressed:(id)sender {
    NSButton *identifyButton = [sender isKindOfClass:NSButton.class] ? sender : nil;
    identifyButton.enabled = NO;
    self.status.stringValue = @"Listening for 20 seconds — press one Kone XP button now…";
    [self.window displayIfNeeded];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSDictionary *result = [self runReader:@[@"identify", @"20"]];
        dispatch_async(dispatch_get_main_queue(), ^{
            identifyButton.enabled = YES;
            if ([result[@"status"] intValue] != 0) {
                self.status.stringValue = result[@"output"];
                return;
            }
            NSArray<NSString *> *parts = [[result[@"output"] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
                componentsSeparatedByString:@"\t"];
            NSInteger button = parts.count == 2 ? [parts[1] integerValue] : 0;
            if (button < 1 || button > 15) {
                self.status.stringValue = @"The raw button event could not be matched to an assignment.";
                return;
            }
            [self.button selectItemAtIndex:button - 1];
            [self readCurrent:nil];
            self.status.stringValue = [NSString stringWithFormat:@"Detected %@. Current action: %@.",
                self.button.selectedItem.title, self.action.selectedItem.title];
        });
    });
}

- (void)saveCurrent:(id)sender {
    (void)sender;
    NSString *slug = self.action.selectedItem.representedObject;
    NSMutableArray<NSString *> *args = [@[@"set-action",
        [NSString stringWithFormat:@"%ld", self.profile.indexOfSelectedItem + 1],
        self.layer.indexOfSelectedItem == 0 ? @"standard" : @"shift",
        [NSString stringWithFormat:@"%ld", self.button.indexOfSelectedItem + 1], slug] mutableCopy];
    if ([slug isEqualToString:@"key"]) [args addObject:self.key.selectedItem.representedObject];
    self.status.stringValue = @"Saving and verifying onboard memory…";
    NSDictionary *result = [self runReader:args];
    if ([result[@"status"] intValue] == 0)
        self.status.stringValue = [NSString stringWithFormat:@"Saved, verified, and Profile %ld activated. The assignment stays in the mouse.", self.profile.indexOfSelectedItem + 1];
    else { self.status.stringValue = result[@"output"]; showAlert(@"Could not save", result[@"output"]); }
}

- (void)backup:(id)sender {
    (void)sender;
    NSString *folder = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents/Kone XP Backups"];
    [[NSFileManager defaultManager] createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
    NSDateFormatter *formatter = [[NSDateFormatter alloc] init]; formatter.dateFormat = @"yyyy-MM-dd-HHmmss";
    NSString *path = [folder stringByAppendingPathComponent:[NSString stringWithFormat:@"kone-xp-%@.json", [formatter stringFromDate:NSDate.date]]];
    NSDictionary *result = [self runReader:@[@"backup", path]];
    self.status.stringValue = [result[@"status"] intValue] == 0
        ? @"Backup saved in Documents › Kone XP Backups." : result[@"output"];
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    self.readerPath = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"konexp-cli"];

    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 560, 430)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Kone XP Configurator"; [self.window center]; NSView *view = self.window.contentView;
    NSTextField *heading = [self label:@"ROCCAT Kone XP — onboard button assignments" frame:NSMakeRect(30, 375, 500, 28)];
    heading.font = [NSFont boldSystemFontOfSize:18]; [view addSubview:heading];
    NSTextField *intro = [self label:@"Choose a profile, layer, physical button, and action. Saving changes only that button and verifies it by reading the mouse back." frame:NSMakeRect(30, 333, 500, 42)];
    intro.maximumNumberOfLines = 2; intro.lineBreakMode = NSLineBreakByWordWrapping; [view addSubview:intro];

    NSArray *fieldLabels = @[@"Profile", @"Layer", @"Physical button", @"Action", @"Keyboard key"];
    NSInteger y[] = {295, 252, 209, 166, 123};
    for (NSInteger i = 0; i < 5; ++i) [view addSubview:[self label:fieldLabels[i] frame:NSMakeRect(30, y[i], 110, 24)]];
    self.profile = [self popup:NSMakeRect(145, 292, 350, 28) labels:@[@"Profile 1", @"Profile 2", @"Profile 3", @"Profile 4", @"Profile 5"]];
    self.layer = [self popup:NSMakeRect(145, 249, 350, 28) labels:@[@"Standard", @"Easy-Shift"]];
    NSArray *buttons = @[@"1 — Left click", @"2 — Right click", @"3 — Wheel click", @"4 — Wheel tilt left", @"5 — Wheel tilt right", @"6 — Scroll up", @"7 — Scroll down", @"8 — T3", @"9 — T4", @"10 — T1", @"11 — T2", @"12 — Top-left DPI up", @"13 — Top-left DPI down", @"14 — Bottom Easy-Shift paddle", @"15 — Top profile button"];
    self.button = [self popup:NSMakeRect(145, 206, 350, 28) labels:buttons];
    self.action = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(145, 163, 350, 28) pullsDown:NO];
    NSArray *actions = @[
        @[@"Disabled", @"disabled"], @[@"Left click", @"left"], @[@"Right click", @"right"], @[@"Middle click", @"middle"],
        @[@"Browser forward", @"forward"], @[@"Browser back", @"back"], @[@"Wheel tilt left", @"tilt-left"], @[@"Wheel tilt right", @"tilt-right"],
        @[@"Scroll up", @"scroll-up"], @[@"Scroll down", @"scroll-down"], @[@"DPI up", @"dpi-up"], @[@"DPI down", @"dpi-down"],
        @[@"Play / pause", @"play-pause"], @[@"Previous track", @"previous-track"], @[@"Next track", @"next-track"],
        @[@"Volume up", @"volume-up"], @[@"Volume down", @"volume-down"], @[@"Cycle profile", @"profile-cycle"],
        @[@"Easy-Shift", @"easy-shift"], @[@"Keyboard key", @"key"]];
    for (NSArray *pair in actions) [self addAction:pair[0] slug:pair[1]];
    self.action.target = self; self.action.action = @selector(actionChanged:);
    self.key = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(145, 120, 350, 28) pullsDown:NO];
    for (NSInteger i = 0; i < 26; ++i) [self addKey:[NSString stringWithFormat:@"%c", (int)('A' + i)] usage:4 + i];
    NSArray *digits = @[@"1", @"2", @"3", @"4", @"5", @"6", @"7", @"8", @"9", @"0"];
    for (NSInteger i = 0; i < 10; ++i) [self addKey:digits[i] usage:30 + i];
    [self addKey:@"Enter" usage:40]; [self addKey:@"Escape" usage:41]; [self addKey:@"Backspace" usage:42]; [self addKey:@"Tab" usage:43]; [self addKey:@"Space" usage:44];
    for (NSInteger i = 0; i < 12; ++i) [self addKey:[NSString stringWithFormat:@"F%ld", i + 1] usage:58 + i];
    [self addKey:@"Right arrow" usage:79]; [self addKey:@"Left arrow" usage:80]; [self addKey:@"Down arrow" usage:81]; [self addKey:@"Up arrow" usage:82]; self.key.enabled = NO;
    for (NSView *control in @[self.profile, self.layer, self.button, self.action, self.key]) [view addSubview:control];

    NSButton *identify = [NSButton buttonWithTitle:@"Identify pressed button" target:self action:@selector(identifyPressed:)]; identify.frame = NSMakeRect(30, 72, 180, 32); [view addSubview:identify];
    NSButton *save = [NSButton buttonWithTitle:@"Save to mouse" target:self action:@selector(saveCurrent:)]; save.frame = NSMakeRect(220, 72, 150, 32); save.keyEquivalent = @"\r"; [view addSubview:save];
    NSButton *backup = [NSButton buttonWithTitle:@"Create backup" target:self action:@selector(backup:)]; backup.frame = NSMakeRect(380, 72, 140, 32); [view addSubview:backup];
    self.status = [self label:@"Ready." frame:NSMakeRect(30, 20, 500, 42)]; self.status.maximumNumberOfLines = 2; self.status.lineBreakMode = NSLineBreakByWordWrapping; [view addSubview:self.status];
    for (NSPopUpButton *control in @[self.profile, self.layer, self.button]) { control.target = self; control.action = @selector(readCurrent:); }
    [self.window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
    NSDictionary *active = [self runReader:@[@"active-profile"]];
    NSInteger activeProfile = [[active[@"output"] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] integerValue];
    if ([active[@"status"] intValue] == 0 && activeProfile >= 1 && activeProfile <= 5)
        [self.profile selectItemAtIndex:activeProfile - 1];
    [self readCurrent:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { (void)sender; return YES; }
@end

int main(void) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        KoneXPDelegate *delegate = [[KoneXPDelegate alloc] init]; app.delegate = delegate; [app run];
    }
    return 0;
}
