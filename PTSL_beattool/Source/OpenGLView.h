#define GL_SILENCE_DEPRECATION
#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>
#include <vector>

class WaveformVisualizer;

@interface OpenGLView : NSOpenGLView
{
    WaveformVisualizer* visualizer;
    NSTimer* renderTimer;
    CVDisplayLinkRef displayLink;
}

@property (nonatomic, assign) BOOL isAnimating;

- (void)updateAudioData:(const std::vector<float>&)samples sampleRate:(double)sampleRate;
- (void)updateBeatData:(double)beatTime tempo:(double)tempo;
- (void)clearBeatData;
- (void)setPlaybackPosition:(double)position;
- (void)setIsPlaying:(BOOL)playing;
- (void)setVisualizationMode:(int)mode;
- (void)startAnimation;
- (void)stopAnimation;
- (void)drawView;

@end