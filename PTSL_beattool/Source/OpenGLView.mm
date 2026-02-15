#define GL_SILENCE_DEPRECATION
#import "OpenGLView.h"
#include "WaveformVisualizer.h"
#include <vector>

// Display link callback
static CVReturn displayLinkCallback(CVDisplayLinkRef displayLink,
                                   const CVTimeStamp* now,
                                   const CVTimeStamp* outputTime,
                                   CVOptionFlags flagsIn,
                                   CVOptionFlags* flagsOut,
                                   void* displayLinkContext) {
    @autoreleasepool {
        OpenGLView* view = (__bridge OpenGLView*)displayLinkContext;
        if (!view) {
            return kCVReturnSuccess;
        }
        
        // Use performSelectorOnMainThread to ensure drawing happens on main thread
        [view performSelectorOnMainThread:@selector(setNeedsDisplay:)
                               withObject:@YES
                            waitUntilDone:NO];
    }
    return kCVReturnSuccess;
}

@implementation OpenGLView

- (instancetype)initWithFrame:(NSRect)frameRect {
    NSLog(@"OpenGLView initWithFrame called");
    // OpenGL 3.2 Core Profile attributes
    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFADoubleBuffer,
        NSOpenGLPFADepthSize, 24,
        NSOpenGLPFAStencilSize, 8,
        NSOpenGLPFAAccelerated,
        NSOpenGLPFAColorSize, 32,
        NSOpenGLPFAAlphaSize, 8,
        0
    };
    
    NSOpenGLPixelFormat* pixelFormat = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    if (!pixelFormat) {
        NSLog(@"Failed to create pixel format");
        return nil;
    }
    
    NSLog(@"Creating NSOpenGLView with pixel format");
    self = [super initWithFrame:frameRect pixelFormat:pixelFormat];
    if (self) {
        NSLog(@"NSOpenGLView created successfully");
        visualizer = new WaveformVisualizer();
        _isAnimating = NO;
        displayLink = NULL;  // Ensure display link is initialized
        renderTimer = nil;
        
        // Enable layer backing for better performance
        [self setWantsBestResolutionOpenGLSurface:YES];
        
        // Don't initialize OpenGL stuff here - wait for prepareOpenGL
        NSLog(@"OpenGLView initialization complete");
    }
    
    return self;
}

- (void)dealloc {
    [self stopAnimation];
    if (renderTimer) {
        [renderTimer invalidate];
        renderTimer = nil;
    }
    if (displayLink) {
        CVDisplayLinkRelease(displayLink);
    }
    delete visualizer;
}

- (void)prepareOpenGL {
    [super prepareOpenGL];
    
    // Make context current
    [[self openGLContext] makeCurrentContext];
    
    // Initialize visualizer now that OpenGL context is ready
    NSRect bounds = [self convertRectToBacking:[self bounds]];
    glViewport(0, 0, bounds.size.width, bounds.size.height);
    
    if (!visualizer->initialize(bounds.size.width, bounds.size.height)) {
        NSLog(@"Failed to initialize visualizer");
    }
    
    // Delay display link setup to avoid startup issues
    NSLog(@"Delaying CVDisplayLink setup");
    
    // Enable V-Sync
    GLint swapInt = 1;
    [[self openGLContext] setValues:&swapInt forParameter:NSOpenGLCPSwapInterval];
    
    // Don't create display link yet - wait for startAnimation
    displayLink = NULL;
}

- (void)reshape {
    [super reshape];
    
    [[self openGLContext] makeCurrentContext];
    NSRect bounds = [self convertRectToBacking:[self bounds]];
    visualizer->resize(bounds.size.width, bounds.size.height);
}

- (void)drawRect:(NSRect)dirtyRect {
    [self drawView];
}

- (void)drawView {
    if (![[self openGLContext] view]) {
        return; // Context not ready
    }
    
    [[self openGLContext] makeCurrentContext];
    CGLLockContext([[self openGLContext] CGLContextObj]);
    
    visualizer->render();
    
    CGLFlushDrawable([[self openGLContext] CGLContextObj]);
    CGLUnlockContext([[self openGLContext] CGLContextObj]);
    
    // Force continuous redraw for animation
    if (_isAnimating) {
        [self setNeedsDisplay:YES];
    }
}

- (void)updateAudioData:(const std::vector<float>&)samples sampleRate:(double)sampleRate {
    if (samples.empty()) {
        return;
    }
    
    visualizer->updateAudioData(samples, sampleRate);
    
    // Force a redraw
    [self setNeedsDisplay:YES];
}

- (void)updateBeatData:(double)beatTime tempo:(double)tempo {
    visualizer->updateBeatData(beatTime, tempo);
}

- (void)clearBeatData {
    visualizer->clearBeatData();
}

- (void)setPlaybackPosition:(double)position {
    visualizer->setPlaybackPosition(position);
}

- (void)setIsPlaying:(BOOL)playing {
    visualizer->setIsPlaying(playing);
}

- (void)setVisualizationMode:(int)mode {
    visualizer->setVisualizationMode((WaveformVisualizer::VisualizationMode)mode);
    // Force a redraw when mode changes
    [self setNeedsDisplay:YES];
}

- (void)startAnimation {
    NSLog(@"startAnimation called, _isAnimating = %d", _isAnimating);
    if (!_isAnimating) {
        NSLog(@"Using NSTimer for animation instead of CVDisplayLink");
        
        // Use NSTimer for animation - more compatible and less problematic
        renderTimer = [NSTimer scheduledTimerWithTimeInterval:1.0/60.0  // 60 FPS
                                                       target:self
                                                     selector:@selector(timerFired:)
                                                     userInfo:nil
                                                      repeats:YES];
        
        // Add to run loop
        [[NSRunLoop currentRunLoop] addTimer:renderTimer forMode:NSRunLoopCommonModes];
        
        _isAnimating = YES;
        NSLog(@"Animation timer started");
    }
    NSLog(@"startAnimation completed");
}

- (void)timerFired:(NSTimer*)timer {
    [self setNeedsDisplay:YES];
}

- (void)stopAnimation {
    if (_isAnimating) {
        if (renderTimer) {
            [renderTimer invalidate];
            renderTimer = nil;
        }
        if (displayLink) {
            CVDisplayLinkStop(displayLink);
        }
        _isAnimating = NO;
    }
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

@end