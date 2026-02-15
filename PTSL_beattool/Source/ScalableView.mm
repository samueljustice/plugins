#import "ScalableView.h"

@implementation ScalableView {
    NSMutableDictionary *originalFontSizes;
}

- (instancetype)initWithFrame:(NSRect)frameRect baseSize:(NSSize)baseSize {
    self = [super initWithFrame:frameRect];
    if (self) {
        baseWidth = baseSize.width;
        baseHeight = baseSize.height;
        scalableSubviews = [[NSMutableArray alloc] init];
        originalFrames = [[NSMutableDictionary alloc] init];
        originalFontSizes = [[NSMutableDictionary alloc] init];
        
        // Enable autoresizing
        [self setAutoresizesSubviews:YES];
        [self setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    }
    return self;
}

- (void)addScalableSubview:(NSView *)view {
    NSLog(@"ScalableView: Adding subview %@", view);
    [super addSubview:view];
    NSLog(@"ScalableView: addSubview completed");
    [scalableSubviews addObject:view];
    NSLog(@"ScalableView: Added to scalableSubviews array");
    
    // Store original frame relative to base size
    NSRect originalFrame = view.frame;
    NSValue *frameValue = [NSValue valueWithRect:originalFrame];
    [originalFrames setObject:frameValue forKey:[NSValue valueWithPointer:(__bridge void *)view]];
    NSLog(@"ScalableView: Stored original frame");
    
    // Disable autoresizing on the subview as we'll handle it manually
    [view setAutoresizingMask:NSViewNotSizable];
    NSLog(@"ScalableView: addScalableSubview completed");
}

- (void)setFrame:(NSRect)frame {
    [super setFrame:frame];
    [self updateScale];
}

- (void)updateScale {
    CGFloat scaleX = self.bounds.size.width / baseWidth;
    CGFloat scaleY = self.bounds.size.height / baseHeight;
    
    // Use the minimum scale to maintain aspect ratio
    CGFloat scale = MIN(scaleX, scaleY);
    
    // Calculate offset to center content if aspect ratio doesn't match
    CGFloat offsetX = (self.bounds.size.width - baseWidth * scale) / 2.0;
    CGFloat offsetY = (self.bounds.size.height - baseHeight * scale) / 2.0;
    
    for (NSView *view in scalableSubviews) {
        NSValue *key = [NSValue valueWithPointer:(__bridge void *)view];
        NSValue *frameValue = [originalFrames objectForKey:key];
        if (frameValue) {
            NSRect originalFrame = [frameValue rectValue];
            
            NSRect newFrame;
            newFrame.origin.x = originalFrame.origin.x * scale + offsetX;
            newFrame.origin.y = originalFrame.origin.y * scale + offsetY;
            newFrame.size.width = originalFrame.size.width * scale;
            newFrame.size.height = originalFrame.size.height * scale;
            
            [view setFrame:newFrame];
            
            // Scale fonts for text elements
            if ([view isKindOfClass:[NSTextField class]]) {
                NSTextField *textField = (NSTextField *)view;
                NSFont *originalFont = textField.font;
                if (originalFont) {
                    NSNumber *storedSize = [originalFontSizes objectForKey:key];
                    CGFloat originalSize;
                    if (!storedSize) {
                        originalSize = [originalFont pointSize];
                        [originalFontSizes setObject:@(originalSize) forKey:key];
                    } else {
                        originalSize = [storedSize floatValue];
                    }
                    CGFloat newSize = originalSize * scale;
                    NSFont *newFont = [NSFont fontWithName:[originalFont fontName] size:newSize];
                    [textField setFont:newFont];
                }
            } else if ([view isKindOfClass:[NSButton class]]) {
                NSButton *button = (NSButton *)view;
                NSFont *originalFont = button.font;
                if (originalFont) {
                    NSNumber *storedSize = [originalFontSizes objectForKey:key];
                    CGFloat originalSize;
                    if (!storedSize) {
                        originalSize = [originalFont pointSize];
                        [originalFontSizes setObject:@(originalSize) forKey:key];
                    } else {
                        originalSize = [storedSize floatValue];
                    }
                    CGFloat newSize = originalSize * scale;
                    NSFont *newFont = [NSFont fontWithName:[originalFont fontName] size:newSize];
                    [button setFont:newFont];
                }
            } else if ([view isKindOfClass:[NSTextView class]]) {
                NSTextView *textView = (NSTextView *)view;
                NSFont *originalFont = textView.font;
                if (originalFont) {
                    NSNumber *storedSize = [originalFontSizes objectForKey:key];
                    CGFloat originalSize;
                    if (!storedSize) {
                        originalSize = [originalFont pointSize];
                        [originalFontSizes setObject:@(originalSize) forKey:key];
                    } else {
                        originalSize = [storedSize floatValue];
                    }
                    CGFloat newSize = originalSize * scale;
                    NSFont *newFont = [NSFont fontWithName:[originalFont fontName] size:newSize];
                    [textView setFont:newFont];
                }
            }
            
            // Special handling for NSScrollView
            if ([view isKindOfClass:[NSScrollView class]]) {
                NSScrollView *scrollView = (NSScrollView *)view;
                NSView *documentView = [scrollView documentView];
                if (documentView) {
                    // Scale the document view's height with the scroll view
                    NSRect docFrame = documentView.frame;
                    docFrame.size.width = newFrame.size.width - 20; // Account for scrollbar
                    docFrame.size.height = newFrame.size.height - 10; // Fill the scrollview height
                    [documentView setFrame:docFrame];
                    
                    // Ensure the text view fills the scroll view
                    if ([documentView isKindOfClass:[NSTextView class]]) {
                        NSTextView *textView = (NSTextView *)documentView;
                        [textView setMinSize:NSMakeSize(0.0, docFrame.size.height)];
                        [textView setMaxSize:NSMakeSize(FLT_MAX, FLT_MAX)];
                        [textView setVerticallyResizable:YES];
                        [textView setHorizontallyResizable:NO];
                        [textView setAutoresizingMask:NSViewWidthSizable];
                        [[textView textContainer] setContainerSize:NSMakeSize(docFrame.size.width, FLT_MAX)];
                        [[textView textContainer] setWidthTracksTextView:YES];
                    }
                }
            }
        }
    }
}

- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];
    
    // Optional: Draw a background to show the scalable area
    [[NSColor controlBackgroundColor] setFill];
    NSRectFill(dirtyRect);
}

@end