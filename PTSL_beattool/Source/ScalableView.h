#import <Cocoa/Cocoa.h>

@interface ScalableView : NSView
{
    CGFloat baseWidth;
    CGFloat baseHeight;
    NSMutableArray *scalableSubviews;
    NSMutableDictionary *originalFrames;
}

- (instancetype)initWithFrame:(NSRect)frameRect baseSize:(NSSize)baseSize;
- (void)addScalableSubview:(NSView *)view;
- (void)updateScale;

@end