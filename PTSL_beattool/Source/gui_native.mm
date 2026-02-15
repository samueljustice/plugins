#import <Cocoa/Cocoa.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include "BeatTracker.h"
#include "PyPTSL.h"
#include "AudioFileReader.h"
#import "OpenGLView.h"
#import "ScalableView.h"

@interface BeatToolController : NSObject <NSApplicationDelegate, NSSplitViewDelegate>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) NSTextField *filePathField;
@property (nonatomic, strong) NSTextField *sampleRateLabel;
@property (nonatomic, strong) NSTextField *bitDepthLabel;
@property (nonatomic, strong) NSTextField *channelsLabel;
@property (nonatomic, strong) NSTextField *durationLabel;
@property (nonatomic, strong) NSTextField *timecodeField;
@property (nonatomic, strong) NSTextField *detectedTempoLabel;
@property (nonatomic, strong) NSButton *downbeatsOnlyCheckbox;
@property (nonatomic, strong) NSButton *clearExistingCheckbox;
@property (nonatomic, strong) NSPopUpButton *timeSignatureDropdown;
@property (nonatomic, strong) NSTextField *minBPMField;
@property (nonatomic, strong) NSTextField *maxBPMField;
@property (nonatomic, strong) NSTextField *hintBPMField;
@property (nonatomic, strong) NSTextField *barOffsetField;
@property (nonatomic, strong) NSButton *showAllBeatsCheckbox;
@property (nonatomic, strong) NSButton *detectButton;
@property (nonatomic, strong) NSButton *sendButton;
@property (nonatomic, strong) NSTextView *logTextView;
@property (nonatomic, strong) NSProgressIndicator *progressBar;
@property (nonatomic, strong) OpenGLView *visualizerView;
@property (nonatomic, strong) NSSplitView *mainSplitView;
@property (nonatomic, strong) ScalableView *upperView;
@property (nonatomic, strong) NSScrollView *logScrollView;

// Beat detection data
@property (nonatomic, assign) double detectedTempo;
@property (nonatomic, strong) NSMutableArray *detectedBeats;
@property (nonatomic, strong) NSMutableArray *detectedBars;

// Audio data for visualization
@property (nonatomic, assign) std::vector<float> audioData;
@property (nonatomic, assign) double audioSampleRate;
@end

@implementation BeatToolController

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    NSLog(@"applicationDidFinishLaunching started");
    
    // Remove problematic stderr redirection that causes crashes
    
    // Create main window (increased height for visualizer)
    NSRect frame = NSMakeRect(0, 0, 1200, 900);
    NSUInteger styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | 
                          NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                               styleMask:styleMask
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
    [self.window setTitle:@"PTSL Beat Tool - Tempo Mapper"];
    [self.window center];
    
    // Set minimum window size
    [self.window setMinSize:NSMakeSize(800, 600)];
    
    // Create main split view
    self.mainSplitView = [[NSSplitView alloc] initWithFrame:frame];
    [self.mainSplitView setDividerStyle:NSSplitViewDividerStyleThin];
    [self.mainSplitView setVertical:NO];
    [self.mainSplitView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [self.mainSplitView setDelegate:self];
    [self.window setContentView:self.mainSplitView];
    
    // Create upper scalable container view for main UI
    self.upperView = [[ScalableView alloc] initWithFrame:NSMakeRect(0, 0, 1200, 750) baseSize:NSMakeSize(1200, 750)];
    [self.upperView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    
    // Create lower view for log (not scalable, just resizable)
    NSView *lowerView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1200, 150)];
    [lowerView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    
    [self.mainSplitView addSubview:self.upperView];
    [self.mainSplitView addSubview:lowerView];
    
    // Set minimum sizes
    [self.mainSplitView setHoldingPriority:250 forSubviewAtIndex:0];
    [self.mainSplitView setHoldingPriority:249 forSubviewAtIndex:1];
    
    ScalableView *scalableView = self.upperView;
    
    // Title
    NSTextField *titleLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 710, 1160, 30)];
    [titleLabel setStringValue:@"Pro Tools Beat Detection & Tempo Mapping"];
    [titleLabel setBezeled:NO];
    [titleLabel setDrawsBackground:NO];
    [titleLabel setEditable:NO];
    [titleLabel setFont:[NSFont boldSystemFontOfSize:18]];
    [titleLabel setAlignment:NSTextAlignmentCenter];
    [scalableView addScalableSubview:titleLabel];
    
    // OpenGL Visualizer View
    NSLog(@"Creating visualizerView");
    self.visualizerView = [[OpenGLView alloc] initWithFrame:NSMakeRect(20, 410, 1160, 280)];
    if (!self.visualizerView) {
        NSLog(@"ERROR: Failed to create visualizerView!");
        return;
    }
    NSLog(@"visualizerView created: %@", self.visualizerView);
    NSLog(@"Adding visualizerView to scalableView");
    [scalableView addScalableSubview:self.visualizerView];
    NSLog(@"visualizerView added to scalableView");
    
    // File selection section
    NSTextField *fileLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 370, 100, 25)];
    [fileLabel setStringValue:@"Audio File:"];
    [fileLabel setBezeled:NO];
    [fileLabel setDrawsBackground:NO];
    [fileLabel setEditable:NO];
    [scalableView addScalableSubview:fileLabel];
    
    self.filePathField = [[NSTextField alloc] initWithFrame:NSMakeRect(130, 370, 820, 25)];
    [self.filePathField setEditable:NO];
    [self.filePathField setToolTip:@"The audio file to analyze for beat detection"];
    [scalableView addScalableSubview:self.filePathField];
    
    NSButton *browseButton = [[NSButton alloc] initWithFrame:NSMakeRect(960, 370, 120, 25)];
    [browseButton setTitle:@"Browse..."];
    [browseButton setTarget:self];
    [browseButton setAction:@selector(browseFile:)];
    [browseButton setToolTip:@"Select an audio file (WAV, AIFF, MP3, M4A)"];
    [scalableView addScalableSubview:browseButton];
    
    // Audio info section
    int infoY = 320;
    NSTextField *infoLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, infoY, 200, 20)];
    [infoLabel setStringValue:@"Audio File Information:"];
    [infoLabel setBezeled:NO];
    [infoLabel setDrawsBackground:NO];
    [infoLabel setEditable:NO];
    [infoLabel setFont:[NSFont boldSystemFontOfSize:13]];
    [scalableView addScalableSubview:infoLabel];
    
    // Sample rate
    NSTextField *srLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(40, infoY - 25, 100, 20)];
    [srLabel setStringValue:@"Sample Rate:"];
    [srLabel setBezeled:NO];
    [srLabel setDrawsBackground:NO];
    [srLabel setEditable:NO];
    [scalableView addScalableSubview:srLabel];
    
    self.sampleRateLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(150, infoY - 25, 150, 20)];
    [self.sampleRateLabel setStringValue:@"Not loaded"];
    [self.sampleRateLabel setBezeled:NO];
    [self.sampleRateLabel setDrawsBackground:NO];
    [self.sampleRateLabel setEditable:NO];
    [scalableView addScalableSubview:self.sampleRateLabel];
    
    // Bit depth
    NSTextField *bdLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(320, infoY - 25, 80, 20)];
    [bdLabel setStringValue:@"Bit Depth:"];
    [bdLabel setBezeled:NO];
    [bdLabel setDrawsBackground:NO];
    [bdLabel setEditable:NO];
    [scalableView addScalableSubview:bdLabel];
    
    self.bitDepthLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(410, infoY - 25, 150, 20)];
    [self.bitDepthLabel setStringValue:@"Not loaded"];
    [self.bitDepthLabel setBezeled:NO];
    [self.bitDepthLabel setDrawsBackground:NO];
    [self.bitDepthLabel setEditable:NO];
    [scalableView addScalableSubview:self.bitDepthLabel];
    
    // Channels
    NSTextField *chLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(40, infoY - 50, 100, 20)];
    [chLabel setStringValue:@"Channels:"];
    [chLabel setBezeled:NO];
    [chLabel setDrawsBackground:NO];
    [chLabel setEditable:NO];
    [scalableView addScalableSubview:chLabel];
    
    self.channelsLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(150, infoY - 50, 150, 20)];
    [self.channelsLabel setStringValue:@"Not loaded"];
    [self.channelsLabel setBezeled:NO];
    [self.channelsLabel setDrawsBackground:NO];
    [self.channelsLabel setEditable:NO];
    [scalableView addScalableSubview:self.channelsLabel];
    
    // Duration
    NSTextField *durLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(320, infoY - 50, 80, 20)];
    [durLabel setStringValue:@"Duration:"];
    [durLabel setBezeled:NO];
    [durLabel setDrawsBackground:NO];
    [durLabel setEditable:NO];
    [scalableView addScalableSubview:durLabel];
    
    self.durationLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(410, infoY - 50, 150, 20)];
    [self.durationLabel setStringValue:@"Not loaded"];
    [self.durationLabel setBezeled:NO];
    [self.durationLabel setDrawsBackground:NO];
    [self.durationLabel setEditable:NO];
    [scalableView addScalableSubview:self.durationLabel];
    
    // Settings section
    int settingsY = 240;
    NSTextField *settingsLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, settingsY, 200, 20)];
    [settingsLabel setStringValue:@"Settings:"];
    [settingsLabel setBezeled:NO];
    [settingsLabel setDrawsBackground:NO];
    [settingsLabel setEditable:NO];
    [settingsLabel setFont:[NSFont boldSystemFontOfSize:13]];
    [scalableView addScalableSubview:settingsLabel];
    
    // Row 1: Start timecode and Time signature
    NSTextField *tcLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(40, settingsY - 35, 120, 20)];
    [tcLabel setStringValue:@"Start Timecode:"];
    [tcLabel setBezeled:NO];
    [tcLabel setDrawsBackground:NO];
    [tcLabel setEditable:NO];
    [scalableView addScalableSubview:tcLabel];
    
    self.timecodeField = [[NSTextField alloc] initWithFrame:NSMakeRect(170, settingsY - 35, 120, 22)];
    [self.timecodeField setStringValue:@"01:00:00:00"];
    [self.timecodeField setToolTip:@"The timecode where the first bar marker will be placed (HH:MM:SS:FF)"];
    [scalableView addScalableSubview:self.timecodeField];
    
    NSTextField *timeSigLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(320, settingsY - 35, 120, 20)];
    [timeSigLabel setStringValue:@"Time Signature:"];
    [timeSigLabel setBezeled:NO];
    [timeSigLabel setDrawsBackground:NO];
    [timeSigLabel setEditable:NO];
    [scalableView addScalableSubview:timeSigLabel];
    
    self.timeSignatureDropdown = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(440, settingsY - 35, 120, 25)];
    [self.timeSignatureDropdown addItemsWithTitles:@[@"4/4", @"3/4", @"6/8", @"5/4", @"7/8"]];
    [self.timeSignatureDropdown selectItemAtIndex:0]; // Default to 4/4
    [self.timeSignatureDropdown setToolTip:@"Time signature determines how beats are grouped into bars"];
    [scalableView addScalableSubview:self.timeSignatureDropdown];
    
    // Detected tempo
    NSTextField *tempoLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(600, settingsY - 35, 120, 20)];
    [tempoLabel setStringValue:@"Detected Tempo:"];
    [tempoLabel setBezeled:NO];
    [tempoLabel setDrawsBackground:NO];
    [tempoLabel setEditable:NO];
    [scalableView addScalableSubview:tempoLabel];
    
    self.detectedTempoLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(730, settingsY - 35, 120, 20)];
    [self.detectedTempoLabel setStringValue:@"Not detected"];
    [self.detectedTempoLabel setBezeled:NO];
    [self.detectedTempoLabel setDrawsBackground:NO];
    [self.detectedTempoLabel setEditable:NO];
    [self.detectedTempoLabel setFont:[NSFont boldSystemFontOfSize:14]];
    [scalableView addScalableSubview:self.detectedTempoLabel];
    
    // Row 2: BPM Range
    NSTextField *bpmRangeLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(40, settingsY - 70, 100, 20)];
    [bpmRangeLabel setStringValue:@"BPM Range:"];
    [bpmRangeLabel setBezeled:NO];
    [bpmRangeLabel setDrawsBackground:NO];
    [bpmRangeLabel setEditable:NO];
    [scalableView addScalableSubview:bpmRangeLabel];
    
    self.minBPMField = [[NSTextField alloc] initWithFrame:NSMakeRect(170, settingsY - 70, 60, 22)];
    [self.minBPMField setStringValue:@"60"];
    [self.minBPMField setPlaceholderString:@"Min"];
    [self.minBPMField setToolTip:@"Minimum expected tempo (beats per minute)"];
    [scalableView addScalableSubview:self.minBPMField];
    
    NSTextField *toLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(235, settingsY - 70, 20, 20)];
    [toLabel setStringValue:@"to"];
    [toLabel setBezeled:NO];
    [toLabel setDrawsBackground:NO];
    [toLabel setEditable:NO];
    [scalableView addScalableSubview:toLabel];
    
    self.maxBPMField = [[NSTextField alloc] initWithFrame:NSMakeRect(260, settingsY - 70, 60, 22)];
    [self.maxBPMField setStringValue:@"180"];
    [self.maxBPMField setPlaceholderString:@"Max"];
    [self.maxBPMField setToolTip:@"Maximum expected tempo (beats per minute)"];
    [scalableView addScalableSubview:self.maxBPMField];
    
    // Hint BPM field
    NSTextField *hintBPMLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(350, settingsY - 70, 80, 20)];
    [hintBPMLabel setStringValue:@"Hint BPM:"];
    [hintBPMLabel setBezeled:NO];
    [hintBPMLabel setDrawsBackground:NO];
    [hintBPMLabel setEditable:NO];
    [scalableView addScalableSubview:hintBPMLabel];
    
    self.hintBPMField = [[NSTextField alloc] initWithFrame:NSMakeRect(440, settingsY - 70, 80, 22)];
    [self.hintBPMField setStringValue:@""];
    [self.hintBPMField setPlaceholderString:@"Optional"];
    [self.hintBPMField setToolTip:@"Optional tempo hint to help the detector lock onto the correct tempo from the start. Useful for tracks with ambiguous or complex rhythms."];
    [scalableView addScalableSubview:self.hintBPMField];
    
    // Bar offset field
    NSTextField *barOffsetLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(550, settingsY - 70, 80, 20)];
    [barOffsetLabel setStringValue:@"Bar Offset:"];
    [barOffsetLabel setBezeled:NO];
    [barOffsetLabel setDrawsBackground:NO];
    [barOffsetLabel setEditable:NO];
    [scalableView addScalableSubview:barOffsetLabel];
    
    self.barOffsetField = [[NSTextField alloc] initWithFrame:NSMakeRect(640, settingsY - 70, 60, 22)];
    [self.barOffsetField setStringValue:@"0"];
    [self.barOffsetField setPlaceholderString:@"0"];
    [self.barOffsetField setToolTip:@"Number of beats to offset before placing bar 1. Use this to align bars with the musical structure if the first detected beat isn't a downbeat."];
    [scalableView addScalableSubview:self.barOffsetField];
    
    // Row 3: Checkboxes
    self.downbeatsOnlyCheckbox = [[NSButton alloc] initWithFrame:NSMakeRect(40, settingsY - 105, 200, 20)];
    [self.downbeatsOnlyCheckbox setButtonType:NSButtonTypeSwitch];
    [self.downbeatsOnlyCheckbox setTitle:@"Bar markers only"];
    [self.downbeatsOnlyCheckbox setState:NSControlStateValueOn];  // Default to checked
    [self.downbeatsOnlyCheckbox setToolTip:@"Create markers only at the start of each bar (downbeats)"];
    [scalableView addScalableSubview:self.downbeatsOnlyCheckbox];
    
    self.clearExistingCheckbox = [[NSButton alloc] initWithFrame:NSMakeRect(250, settingsY - 105, 200, 20)];
    [self.clearExistingCheckbox setButtonType:NSButtonTypeSwitch];
    [self.clearExistingCheckbox setTitle:@"Clear existing markers"];
    [self.clearExistingCheckbox setState:NSControlStateValueOn];
    [self.clearExistingCheckbox setToolTip:@"Remove existing memory locations before creating new ones"];
    [scalableView addScalableSubview:self.clearExistingCheckbox];
    
    // Show all beats checkbox
    self.showAllBeatsCheckbox = [[NSButton alloc] initWithFrame:NSMakeRect(470, settingsY - 105, 200, 20)];
    [self.showAllBeatsCheckbox setButtonType:NSButtonTypeSwitch];
    [self.showAllBeatsCheckbox setTitle:@"Show all beats"];
    [self.showAllBeatsCheckbox setState:NSControlStateValueOff];
    [self.showAllBeatsCheckbox setToolTip:@"Export all detected beats instead of just bar markers. Useful for debugging alignment issues and verifying beat detection accuracy."];
    [scalableView addScalableSubview:self.showAllBeatsCheckbox];
    
    
    // Action buttons
    self.detectButton = [[NSButton alloc] initWithFrame:NSMakeRect(400, 90, 130, 30)];
    [self.detectButton setTitle:@"Detect Beats"];
    [self.detectButton setTarget:self];
    [self.detectButton setAction:@selector(detectBeats:)];
    [self.detectButton setEnabled:NO];
    [self.detectButton setBezelStyle:NSBezelStyleRounded];
    [self.detectButton setToolTip:@"Analyze the audio file and detect beat locations"];
    [scalableView addScalableSubview:self.detectButton];
    
    self.sendButton = [[NSButton alloc] initWithFrame:NSMakeRect(550, 90, 150, 30)];
    [self.sendButton setTitle:@"Send to Pro Tools"];
    [self.sendButton setTarget:self];
    [self.sendButton setAction:@selector(sendToProTools:)];
    [self.sendButton setEnabled:NO];
    [self.sendButton setBezelStyle:NSBezelStyleRounded];
    [self.sendButton setToolTip:@"Create bar markers in Pro Tools at detected beat locations"];
    [scalableView addScalableSubview:self.sendButton];
    
    // Progress bar
    self.progressBar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 50, 1160, 20)];
    [self.progressBar setStyle:NSProgressIndicatorStyleBar];
    [self.progressBar setIndeterminate:YES];
    [self.progressBar setDisplayedWhenStopped:NO];
    [scalableView addScalableSubview:self.progressBar];
    
    // Log window in lower view
    NSTextField *logLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(10, lowerView.frame.size.height - 25, 200, 20)];
    [logLabel setStringValue:@"Processing Log:"];
    [logLabel setBezeled:NO];
    [logLabel setDrawsBackground:NO];
    [logLabel setEditable:NO];
    [logLabel setAutoresizingMask:NSViewMinXMargin | NSViewMaxXMargin | NSViewMinYMargin];
    [lowerView addSubview:logLabel];
    
    self.logScrollView = [[NSScrollView alloc] initWithFrame:NSMakeRect(10, 10, lowerView.frame.size.width - 20, lowerView.frame.size.height - 35)];
    [self.logScrollView setBorderType:NSBezelBorder];
    [self.logScrollView setHasVerticalScroller:YES];
    [self.logScrollView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    
    self.logTextView = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, self.logScrollView.frame.size.width, self.logScrollView.frame.size.height)];
    [self.logTextView setEditable:NO];
    [self.logTextView setFont:[NSFont fontWithName:@"Menlo" size:11]];
    [self.logTextView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    
    // Support dark mode
    [self.logTextView setBackgroundColor:[NSColor textBackgroundColor]];
    [self.logTextView setTextColor:[NSColor labelColor]];
    [self.logTextView setInsertionPointColor:[NSColor labelColor]];
    
    [self.logScrollView setDocumentView:self.logTextView];
    [lowerView addSubview:self.logScrollView];
    
    NSLog(@"Making window key and front");
    [self.window makeKeyAndOrderFront:nil];
    NSLog(@"Window shown");
    
    // Don't start animation automatically - let user trigger it
    // [self.visualizerView startAnimation];
    NSLog(@"Skipping automatic visualizer animation");
    
    // Initialize
    self.detectedBeats = [[NSMutableArray alloc] init];
    self.detectedBars = [[NSMutableArray alloc] init];
    NSLog(@"Arrays initialized");
    [self log:@"PTSL Beat Tool initialized"];
    NSLog(@"First log completed");
    [self log:@"Using Beat-and-Tempo-Tracking for variable tempo detection"];
    NSLog(@"Second log completed");
    NSLog(@"applicationDidFinishLaunching completed");
}

- (void)browseFile:(id)sender {
    NSOpenPanel *openPanel = [NSOpenPanel openPanel];
    [openPanel setAllowedFileTypes:@[@"wav", @"aif", @"aiff", @"mp3", @"m4a"]];
    [openPanel setAllowsMultipleSelection:NO];
    
    if ([openPanel runModal] == NSModalResponseOK) {
        NSURL *url = [[openPanel URLs] objectAtIndex:0];
        NSString *path = [url path];
        [self.filePathField setStringValue:path];
        [self loadAudioInfo:path];
        [self.detectButton setEnabled:YES];
        [self log:[NSString stringWithFormat:@"Audio file selected: %@", [path lastPathComponent]]];
    }
}

- (void)loadAudioInfo:(NSString *)path {
    [self log:@"\n=================================================="];
    [self log:[NSString stringWithFormat:@"Loading audio file: %@", [path lastPathComponent]]];
    
    AudioFileReader reader;
    if (reader.load([path UTF8String])) {
        // Update UI with audio info
        float sampleRate = reader.getSampleRate();
        double duration = reader.getDuration();
        int channels = reader.getChannels();
        int bitsPerSample = reader.getBitsPerSample();
        
        [self.sampleRateLabel setStringValue:[NSString stringWithFormat:@"%.0f Hz", sampleRate]];
        [self.channelsLabel setStringValue:[NSString stringWithFormat:@"%d (%@)", 
            channels, channels == 1 ? @"Mono" : @"Stereo"]];
        
        int minutes = (int)(duration / 60);
        int seconds = (int)duration % 60;
        [self.durationLabel setStringValue:[NSString stringWithFormat:@"%d:%02d", minutes, seconds]];
        
        [self.bitDepthLabel setStringValue:[NSString stringWithFormat:@"%d bit", bitsPerSample]];
        
        [self log:@"Audio file loaded successfully!"];
        
        // Store audio data for visualizer
        self.audioData = reader.getMonoAudio();
        self.audioSampleRate = sampleRate;
        
        // Update visualizer with loaded audio
        if (self.visualizerView) {
            [self.visualizerView updateAudioData:self.audioData sampleRate:self.audioSampleRate];
        }
        [self log:[NSString stringWithFormat:@"  Sample Rate: %.0f Hz", sampleRate]];
        [self log:[NSString stringWithFormat:@"  Bit Depth: %d bit", bitsPerSample]];
        [self log:[NSString stringWithFormat:@"  Channels: %d (%@)", channels, channels == 1 ? @"Mono" : @"Stereo"]];
        [self log:[NSString stringWithFormat:@"  Duration: %d:%02d", minutes, seconds]];
        
        // Check audio data size
        std::vector<float> monoAudio = reader.getMonoAudio();
        [self log:[NSString stringWithFormat:@"  Audio samples: %zu", monoAudio.size()]];
    } else {
        [self log:@"ERROR: Failed to load audio file!"];
        [self log:@"Please check if the file is a valid WAV file."];
        
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Failed to Load Audio File"];
        [alert setInformativeText:@"Could not read the audio file. Please ensure it's a valid WAV file."];
        [alert setAlertStyle:NSAlertStyleWarning];
        [alert runModal];
    }
}

- (BeatTracker::TimeSignature)getSelectedTimeSignature {
    NSInteger index = [self.timeSignatureDropdown indexOfSelectedItem];
    switch (index) {
        case 0: return BeatTracker::TIME_4_4;
        case 1: return BeatTracker::TIME_3_4;
        case 2: return BeatTracker::TIME_6_8;
        case 3: return BeatTracker::TIME_5_4;
        case 4: return BeatTracker::TIME_7_8;
        default: return BeatTracker::TIME_4_4;
    }
}

- (void)detectBeats:(id)sender {
    NSString *filepath = [self.filePathField stringValue];
    if ([filepath length] == 0) {
        [self log:@"ERROR: No file selected"];
        return;
    }
    
    [self log:@"\n=================================================="];
    [self log:@"Starting beat detection..."];
    [self log:[NSString stringWithFormat:@"File: %@", [filepath lastPathComponent]]];
    [self log:@"Using Beat-and-Tempo-Tracking for variable tempo detection"];
    [self log:[NSString stringWithFormat:@"Time signature: %@", 
               [[self.timeSignatureDropdown selectedItem] title]]];
    
    // Get BPM range
    double minBPM = [[self.minBPMField stringValue] doubleValue];
    double maxBPM = [[self.maxBPMField stringValue] doubleValue];
    if (minBPM <= 0) minBPM = 60;
    if (maxBPM <= 0) maxBPM = 180;
    if (minBPM > maxBPM) {
        double temp = minBPM;
        minBPM = maxBPM;
        maxBPM = temp;
    }
    [self log:[NSString stringWithFormat:@"BPM Range: %.0f - %.0f", minBPM, maxBPM]];
    
    [self.progressBar startAnimation:nil];
    [self.detectButton setEnabled:NO];
    
    // Run in background
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        @try {
            [self detectBeatsWithNewTracker:filepath];
        }
        @catch (NSException *exception) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self log:@"CRITICAL ERROR: Exception during beat detection"];
                [self log:[NSString stringWithFormat:@"Exception: %@", exception.name]];
                [self log:[NSString stringWithFormat:@"Reason: %@", exception.reason]];
                [self log:@"This is likely a crash in the beat tracking library"];
                
                NSAlert *alert = [[NSAlert alloc] init];
                [alert setMessageText:@"Beat Detection Crashed"];
                [alert setInformativeText:[NSString stringWithFormat:@"An exception occurred: %@\n\nPlease check the log for details.", exception.reason]];
                [alert setAlertStyle:NSAlertStyleCritical];
                [alert runModal];
            });
        }
        @finally {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self.progressBar stopAnimation:nil];
                [self.detectButton setEnabled:YES];
            });
        }
    });
}

- (void)detectBeatsWithNewTracker:(NSString *)filepath {
    AudioFileReader reader;
    if (!reader.load([filepath UTF8String])) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self log:@"ERROR: Failed to load audio file!"];
            NSAlert *alert = [[NSAlert alloc] init];
            [alert setMessageText:@"Failed to Load Audio File"];
            [alert setInformativeText:@"Could not read the audio file."];
            [alert runModal];
        });
        return;
    }
    
    dispatch_async(dispatch_get_main_queue(), ^{
        [self log:@"Audio loaded successfully"];
        [self log:@"Initializing Beat-and-Tempo-Tracking..."];
        // Clear previous beat data in visualizer
        if (self.visualizerView) {
            [self.visualizerView clearBeatData];
            // Start animation if not already running
            if (!self.visualizerView.isAnimating) {
                [self.visualizerView startAnimation];
            }
            // Switch to analysis mode
            [self.visualizerView setVisualizationMode:1]; // MODE_ANALYZING
        }
    });
    
    // Get audio data
    std::vector<float> audioData = reader.getMonoAudio();
    double sampleRate = reader.getSampleRate();
    
    // Log audio data info for debugging
    dispatch_async(dispatch_get_main_queue(), ^{
        [self log:[NSString stringWithFormat:@"Audio data: %zu samples at %.0f Hz", 
                   audioData.size(), sampleRate]];
    });
    
    // Create beat tracker
    BeatTracker* tracker = new BeatTracker();
    
    if (!tracker) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self log:@"ERROR: Failed to create BeatTracker!"];
        });
        return;
    }
    
    // Set BPM range
    double minBPM = [[self.minBPMField stringValue] doubleValue];
    double maxBPM = [[self.maxBPMField stringValue] doubleValue];
    if (minBPM <= 0) minBPM = 60;
    if (maxBPM <= 0) maxBPM = 180;
    
    dispatch_async(dispatch_get_main_queue(), ^{
        [self log:[NSString stringWithFormat:@"BPM range: %.0f - %.0f", minBPM, maxBPM]];
    });
    
    tracker->setMinTempo(minBPM);
    tracker->setMaxTempo(maxBPM);
    
    // Set hint BPM if provided
    double hintBPM = [[self.hintBPMField stringValue] doubleValue];
    if (hintBPM > 0) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self log:[NSString stringWithFormat:@"Using tempo hint: %.0f BPM", hintBPM]];
        });
        tracker->setInitialTempo(hintBPM);
    }
    
    // Set bar offset
    int barOffset = [[self.barOffsetField stringValue] intValue];
    if (barOffset != 0) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self log:[NSString stringWithFormat:@"Bar offset: %d beats", barOffset]];
        });
        tracker->setBarOffset(barOffset);
    }
    
    // Set progress callback
    tracker->setProgressCallback([self](const std::string& message) {
        // Create a copy of the message to ensure it's valid when the block executes
        std::string messageCopy = message;
        dispatch_async(dispatch_get_main_queue(), ^{
            // Safely convert the C++ string to NSString
            NSString *logMessage = nil;
            if (!messageCopy.empty()) {
                const char* cStr = messageCopy.c_str();
                if (cStr != nullptr) {
                    logMessage = [NSString stringWithUTF8String:cStr];
                }
            }
            
            // If conversion failed, create a fallback message
            if (logMessage == nil) {
                logMessage = @"[Progress update]";
            }
            
            [self log:logMessage];
        });
    });
    
    // Get selected time signature
    BeatTracker::TimeSignature timeSig = [self getSelectedTimeSignature];
    
    // Process audio
    dispatch_async(dispatch_get_main_queue(), ^{
        [self log:@"Starting audio processing..."];
    });
    
    bool success = false;
    try {
        success = tracker->processAudio(audioData, sampleRate, timeSig);
    } catch (const std::exception& e) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self log:[NSString stringWithFormat:@"C++ Exception: %s", e.what()]];
        });
    } catch (...) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self log:@"Unknown C++ exception occurred"];
        });
    }
    
    if (success) {
        // Get results
        const std::vector<BeatTracker::Bar>& bars = tracker->getBars();
        const std::vector<BeatTracker::Beat>& beats = tracker->getBeats();
        self.detectedTempo = tracker->getAverageTempo();
        
        // Clear previous results
        [self.detectedBeats removeAllObjects];
        [self.detectedBars removeAllObjects];
        
        // Convert all beats to our format
        for (const auto& beat : beats) {
            NSDictionary *beatDict = @{
                @"time": @(beat.position_seconds),
                @"confidence": @(1.0),
                @"isDownbeat": @(beat.is_downbeat),
                @"tempo": @(beat.tempo_at_beat)
            };
            [self.detectedBeats addObject:beatDict];
        }
        
        // Copy beat data for safe access in dispatch block
        std::vector<std::pair<double, double>> beatData;
        for (const auto& beat : beats) {
            beatData.push_back(std::make_pair(beat.position_seconds, beat.tempo_at_beat));
        }
        
        // Update visualizer with all beat data
        // Copy audio data and capture visualizer before the block
        std::vector<float> audioDataCopy = self.audioData;
        double sampleRateCopy = self.audioSampleRate;
        
        // Make a copy of the beatData to ensure it's valid in the block
        std::vector<std::pair<double, double>> beatDataCopy = beatData;
        
        dispatch_async(dispatch_get_main_queue(), ^{
            // Get the visualizer from self inside the block
            if (!self || !self.visualizerView) {
                NSLog(@"ERROR: self or visualizerView is nil in beat data update block");
                return;
            }
            
            @try {
                // Update visualizer with audio data if we have it
                if (!audioDataCopy.empty()) {
                    [self.visualizerView updateAudioData:audioDataCopy sampleRate:sampleRateCopy];
                }
                
                // Send beat positions to visualizer for highlighting
                if (!beatDataCopy.empty()) {
                    for (const auto& beatPair : beatDataCopy) {
                        [self.visualizerView updateBeatData:beatPair.first tempo:beatPair.second];
                    }
                }
            }
            @catch (NSException *exception) {
                NSLog(@"Exception updating visualizer: %@ - %@", exception.name, exception.reason);
                [self log:[NSString stringWithFormat:@"ERROR: Exception in visualizer update: %@", exception.reason]];
            }
        });
        
        // Convert bars to our format
        for (const auto& bar : bars) {
            NSDictionary *barDict = @{
                @"time": @(bar.position_seconds),
                @"bpm": @(bar.bpm),
                @"barNumber": @(bar.bar_number)
            };
            [self.detectedBars addObject:barDict];
        }
        
        dispatch_async(dispatch_get_main_queue(), ^{
            [self updateAfterDetection];
            // Switch back to ambient mode
            if (self.visualizerView) {
                [self.visualizerView setVisualizationMode:0]; // MODE_AMBIENT
            }
        });
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self log:@"ERROR: Beat tracking failed!"];
            // Switch back to ambient mode
            if (self.visualizerView) {
                [self.visualizerView setVisualizationMode:0]; // MODE_AMBIENT
            }
            NSAlert *alert = [[NSAlert alloc] init];
            [alert setMessageText:@"Beat Tracking Failed"];
            [alert setInformativeText:@"Could not track beats in the audio file."];
            [alert runModal];
        });
    }
    
    delete tracker;
}

- (void)updateAfterDetection {
    [self.detectedTempoLabel setStringValue:[NSString stringWithFormat:@"%.1f BPM avg", self.detectedTempo]];
    [self log:@"\nBeat tracking complete!"];
    [self log:[NSString stringWithFormat:@"Average tempo: %.1f BPM", self.detectedTempo]];
    [self log:[NSString stringWithFormat:@"Found %lu bars", [self.detectedBars count]]];
    
    // Show first few bars with their tempos
    [self log:@"\nFirst 10 bars with BPM:"];
    for (int i = 0; i < MIN(10, [self.detectedBars count]); i++) {
        NSDictionary *bar = self.detectedBars[i];
        double time = [bar[@"time"] doubleValue];
        double bpm = [bar[@"bpm"] doubleValue];
        int barNum = [bar[@"barNumber"] intValue];
        
        [self log:[NSString stringWithFormat:@"  Bar %d: %.3fs - %.1f BPM", barNum, time, bpm]];
    }
    
    if ([self.detectedBars count] > 10) {
        [self log:[NSString stringWithFormat:@"  ... and %lu more bars", 
                  [self.detectedBars count] - 10]];
    }
    
    [self.sendButton setEnabled:YES];
}

- (void)sendToProTools:(id)sender {
    [self log:@"\n=================================================="];
    [self log:@"Connecting to Pro Tools..."];
    [self.progressBar startAnimation:nil];
    
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        std::vector<PyPTSL::BarMarker> markers;
        BOOL showAllBeats = ([self.showAllBeatsCheckbox state] == NSControlStateValueOn);
        
        if (showAllBeats) {
            // Send ALL beats as markers for debugging
            dispatch_async(dispatch_get_main_queue(), ^{
                [self log:@"Sending ALL beats as markers (debug mode)..."];
            });
            
            int beatNum = 1;
            for (NSDictionary *beat in self.detectedBeats) {
                PyPTSL::BarMarker marker;
                marker.time = [beat[@"time"] doubleValue];
                marker.bpm = [beat[@"tempo"] doubleValue];
                marker.barNumber = beatNum++; // Beat number instead of bar number
                markers.push_back(marker);
            }
        } else {
            // Convert bars to format expected by PyPTSL
            for (NSDictionary *bar in self.detectedBars) {
                PyPTSL::BarMarker marker;
                marker.time = [bar[@"time"] doubleValue];
                marker.bpm = [bar[@"bpm"] doubleValue];
                marker.barNumber = [bar[@"barNumber"] intValue];
                markers.push_back(marker);
            }
        }
        
        PyPTSL pyPTSL;
        NSString *timecode = [self.timecodeField stringValue];
        BOOL clearExisting = ([self.clearExistingCheckbox state] == NSControlStateValueOn);
        
        dispatch_async(dispatch_get_main_queue(), ^{
            [self log:[NSString stringWithFormat:@"WAV file starts at timecode: %@", timecode]];
            if (clearExisting) {
                [self log:@"Clearing existing memory locations..."];
            }
            if (showAllBeats) {
                [self log:[NSString stringWithFormat:@"Creating %lu beat markers...", markers.size()]];
            } else {
                [self log:@"Creating bar markers with BPM labels..."];
            }
        });
        
        if (pyPTSL.sendBarsToProTools(markers, 
                                      [timecode UTF8String], 
                                      clearExisting)) {
            dispatch_async(dispatch_get_main_queue(), ^{
                // Show marker creation progress
                int markerCount = [self.detectedBars count];
                
                [self log:[NSString stringWithFormat:@"Creating %d bar markers...", markerCount]];
                
                // Show first few markers
                int shown = 0;
                for (int i = 0; i < [self.detectedBars count] && shown < 5; i++) {
                    NSDictionary *bar = self.detectedBars[i];
                    double barTime = [bar[@"time"] doubleValue];
                    double bpm = [bar[@"bpm"] doubleValue];
                    int barNum = [bar[@"barNumber"] intValue];
                    NSString *markerTC = [self timecodeAddSeconds:timecode seconds:barTime];
                    NSString *name = [NSString stringWithFormat:@"Bar %d - %.1f BPM", barNum, bpm];
                    [self log:[NSString stringWithFormat:@"  Created: %@ at %@", name, markerTC]];
                    shown++;
                }
                
                if (markerCount > 5) {
                    [self log:[NSString stringWithFormat:@"  ... and %d more markers", markerCount - 5]];
                }
                
                [self log:@"\nSuccessfully created tempo markers!"];
                
                // Show timing info
                if ([self.detectedBeats count] > 0) {
                    double firstBeatTime = [self.detectedBeats[0][@"time"] doubleValue];
                    double lastBeatTime = [self.detectedBeats.lastObject[@"time"] doubleValue];
                    NSString *endTC = [self timecodeAddSeconds:timecode seconds:lastBeatTime];
                    [self log:[NSString stringWithFormat:@"Markers span from %@ to %@", timecode, endTC]];
                }
                
                NSAlert *alert = [[NSAlert alloc] init];
                [alert setMessageText:@"Success"];
                [alert setInformativeText:@"Tempo markers created in Pro Tools!"];
                [alert setAlertStyle:NSAlertStyleInformational];
                [alert runModal];
            });
        } else {
            std::string error = pyPTSL.getLastError();
            dispatch_async(dispatch_get_main_queue(), ^{
                [self log:[NSString stringWithFormat:@"Error: %s", error.c_str()]];
                NSAlert *alert = [[NSAlert alloc] init];
                [alert setMessageText:@"Pro Tools Connection Error"];
                [alert setInformativeText:[NSString stringWithUTF8String:error.c_str()]];
                [alert setAlertStyle:NSAlertStyleCritical];
                [alert runModal];
            });
        }
        
        dispatch_async(dispatch_get_main_queue(), ^{
            [self.progressBar stopAnimation:nil];
        });
    });
}

- (NSString *)timecodeAddSeconds:(NSString *)baseTC seconds:(double)seconds {
    // Note: The actual frame rate is handled by py-ptsl when creating markers
    // This is just for display in the log. The Python script will use the
    // actual session frame rate when creating markers in Pro Tools
    
    // Parse base timecode
    NSArray *parts = [baseTC componentsSeparatedByString:@":"];
    int hours = [parts[0] intValue];
    int minutes = [parts[1] intValue];
    int secs = [parts[2] intValue];
    int frames = parts.count > 3 ? [parts[3] intValue] : 0;
    
    int frameRate = 30; // Display default - actual session frame rate used by py-ptsl
    
    // Convert to total frames
    int totalFrames = (hours * 3600 + minutes * 60 + secs) * frameRate + frames;
    
    // Add the seconds as frames
    totalFrames += (int)(seconds * frameRate);
    
    // Convert back to timecode
    hours = totalFrames / (3600 * frameRate);
    int remaining = totalFrames % (3600 * frameRate);
    minutes = remaining / (60 * frameRate);
    remaining = remaining % (60 * frameRate);
    secs = remaining / frameRate;
    frames = remaining % frameRate;
    
    return [NSString stringWithFormat:@"%02d:%02d:%02d:%02d", hours, minutes, secs, frames];
}

- (void)log:(NSString *)message {
    // Create attributed string with proper text color for dark mode
    NSDictionary *attributes = @{
        NSForegroundColorAttributeName: [NSColor labelColor],
        NSFontAttributeName: [NSFont fontWithName:@"Menlo" size:11]
    };
    
    NSAttributedString *line = [[NSAttributedString alloc] 
        initWithString:[NSString stringWithFormat:@"%@\n", message]
        attributes:attributes];
    
    [[self.logTextView textStorage] appendAttributedString:line];
    [self.logTextView scrollToEndOfDocument:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}

#pragma mark - NSSplitViewDelegate

- (CGFloat)splitView:(NSSplitView *)splitView constrainMinCoordinate:(CGFloat)proposedMinimumPosition ofSubviewAt:(NSInteger)dividerIndex {
    // Minimum height for upper view
    return 400;
}

- (CGFloat)splitView:(NSSplitView *)splitView constrainMaxCoordinate:(CGFloat)proposedMaximumPosition ofSubviewAt:(NSInteger)dividerIndex {
    // Minimum height for lower view (log)
    return splitView.frame.size.height - 100;
}

- (BOOL)splitView:(NSSplitView *)splitView canCollapseSubview:(NSView *)subview {
    // Don't allow collapsing
    return NO;
}


@end

int main(int argc, char *argv[]) {
    NSLog(@"main() started");
    @autoreleasepool {
        NSLog(@"Creating NSApplication");
        NSApplication *app = [NSApplication sharedApplication];
        NSLog(@"Creating BeatToolController");
        BeatToolController *controller = [[BeatToolController alloc] init];
        NSLog(@"Setting delegate");
        [app setDelegate:controller];
        NSLog(@"Running app");
        [app run];
    }
    return 0;
}