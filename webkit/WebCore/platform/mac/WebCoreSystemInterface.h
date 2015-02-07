

#ifndef WebCoreSystemInterface_h
#define WebCoreSystemInterface_h

#include <ApplicationServices/ApplicationServices.h>
#include <objc/objc.h>

typedef struct _NSRange NSRange;

#ifdef NSGEOMETRY_TYPES_SAME_AS_CGGEOMETRY_TYPES
typedef struct CGPoint NSPoint;
typedef struct CGRect NSRect;
#else
typedef struct _NSPoint NSPoint;
typedef struct _NSRect NSRect;
#endif

#ifdef __OBJC__
@class NSArray;
@class NSButtonCell;
@class NSData;
@class NSDate;
@class NSEvent;
@class NSFont;
@class NSImage;
@class NSMenu;
@class NSMutableArray;
@class NSMutableURLRequest;
@class NSString;
@class NSTextFieldCell;
@class NSURLConnection;
@class NSURLRequest;
@class NSURLResponse;
@class NSView;
@class QTMovie;
@class QTMovieView;
#else
typedef struct NSArray NSArray;
typedef struct NSButtonCell NSButtonCell;
typedef struct NSData NSData;
typedef struct NSDate NSDate;
typedef struct NSEvent NSEvent;
typedef struct NSFont NSFont;
typedef struct NSImage NSImage;
typedef struct NSMenu NSMenu;
typedef struct NSMutableArray NSMutableArray;
typedef struct NSMutableURLRequest NSMutableURLRequest;
typedef struct NSURLRequest NSURLRequest;
typedef struct NSString NSString;
typedef struct NSTextFieldCell NSTextFieldCell;
typedef struct NSURLConnection NSURLConnection;
typedef struct NSURLResponse NSURLResponse;
typedef struct NSView NSView;
typedef struct objc_object *id;
typedef struct QTMovie QTMovie;
typedef struct QTMovieView QTMovieView;
#endif

#ifdef __cplusplus
extern "C" {
#endif

// In alphabetical order.

extern void (*wkAdvanceDefaultButtonPulseAnimation)(NSButtonCell *);
extern BOOL (*wkCGContextGetShouldSmoothFonts)(CGContextRef);
extern CFReadStreamRef (*wkCreateCustomCFReadStream)(void *(*formCreate)(CFReadStreamRef, void *), 
    void (*formFinalize)(CFReadStreamRef, void *), 
    Boolean (*formOpen)(CFReadStreamRef, CFStreamError *, Boolean *, void *), 
    CFIndex (*formRead)(CFReadStreamRef, UInt8 *, CFIndex, CFStreamError *, Boolean *, void *), 
    Boolean (*formCanRead)(CFReadStreamRef, void *), 
    void (*formClose)(CFReadStreamRef, void *), 
    void (*formSchedule)(CFReadStreamRef, CFRunLoopRef, CFStringRef, void *), 
    void (*formUnschedule)(CFReadStreamRef, CFRunLoopRef, CFStringRef, void *),
    void *context);
extern id (*wkCreateNSURLConnectionDelegateProxy)(void);
extern void (*wkDrawBezeledTextFieldCell)(NSRect, BOOL enabled);
extern void (*wkDrawTextFieldCellFocusRing)(NSTextFieldCell*, NSRect);
extern void (*wkDrawCapsLockIndicator)(CGContextRef, CGRect);
extern void (*wkDrawBezeledTextArea)(NSRect, BOOL enabled);
extern void (*wkDrawFocusRing)(CGContextRef, CGColorRef, int radius);
extern NSFont* (*wkGetFontInLanguageForRange)(NSFont*, NSString*, NSRange);
extern NSFont* (*wkGetFontInLanguageForCharacter)(NSFont*, UniChar);
extern BOOL (*wkGetGlyphTransformedAdvances)(CGFontRef, NSFont*, CGAffineTransform*, ATSGlyphRef*, CGSize* advance);
extern void (*wkDrawMediaSliderTrack)(int themeStyle, CGContextRef context, CGRect rect, float timeLoaded, float currentTime, 
    float duration, unsigned state);
extern void (*wkDrawMediaUIPart)(int part, int themeStyle, CGContextRef context, CGRect rect, unsigned state);
extern NSString* (*wkGetPreferredExtensionForMIMEType)(NSString*);
extern NSArray* (*wkGetExtensionsForMIMEType)(NSString*);
extern NSString* (*wkGetMIMETypeForExtension)(NSString*);
extern ATSUFontID (*wkGetNSFontATSUFontId)(NSFont*);
extern double (*wkGetNSURLResponseCalculatedExpiration)(NSURLResponse *response);
extern NSDate *(*wkGetNSURLResponseLastModifiedDate)(NSURLResponse *response);
extern BOOL (*wkGetNSURLResponseMustRevalidate)(NSURLResponse *response);
extern void (*wkGetWheelEventDeltas)(NSEvent*, float* deltaX, float* deltaY, BOOL* continuous);
extern BOOL (*wkHitTestMediaUIPart)(int part, int themeStyle, CGRect bounds, CGPoint point);
extern void (*wkMeasureMediaUIPart)(int part, int themeStyle, CGRect *bounds, CGSize *naturalSize);
extern BOOL (*wkMediaControllerThemeAvailable)(int themeStyle);
extern void (*wkPopupMenu)(NSMenu*, NSPoint location, float width, NSView*, int selectedItem, NSFont*);
extern unsigned (*wkQTIncludeOnlyModernMediaFileTypes)(void);
extern int (*wkQTMovieDataRate)(QTMovie*);
extern float (*wkQTMovieMaxTimeLoaded)(QTMovie*);
extern NSString *(*wkQTMovieMaxTimeLoadedChangeNotification)(void);
extern float (*wkQTMovieMaxTimeSeekable)(QTMovie*);
extern int (*wkQTMovieGetType)(QTMovie* movie);
extern BOOL (*wkQTMovieHasClosedCaptions)(QTMovie* movie);
extern void (*wkQTMovieSetShowClosedCaptions)(QTMovie* movie, BOOL showClosedCaptions);
extern void (*wkQTMovieViewSetDrawSynchronously)(QTMovieView*, BOOL);
extern void (*wkSetCGFontRenderingMode)(CGContextRef, NSFont*);
extern void (*wkSetDragImage)(NSImage*, NSPoint offset);
extern void (*wkSetNSURLConnectionDefersCallbacks)(NSURLConnection *, BOOL);
extern void (*wkSetNSURLRequestShouldContentSniff)(NSMutableURLRequest *, BOOL);
extern void (*wkSetPatternBaseCTM)(CGContextRef, CGAffineTransform);
extern void (*wkSetPatternPhaseInUserSpace)(CGContextRef, CGPoint);
extern CGAffineTransform (*wkGetUserToBaseCTM)(CGContextRef);
extern void (*wkSetUpFontCache)();
extern void (*wkSignalCFReadStreamEnd)(CFReadStreamRef stream);
extern void (*wkSignalCFReadStreamError)(CFReadStreamRef stream, CFStreamError *error);
extern void (*wkSignalCFReadStreamHasBytes)(CFReadStreamRef stream);
extern unsigned (*wkInitializeMaximumHTTPConnectionCountPerHost)(unsigned preferredConnectionCount);
extern void (*wkSetCONNECTProxyForStream)(CFReadStreamRef, CFStringRef proxyHost, CFNumberRef proxyPort);
extern void (*wkSetCONNECTProxyAuthorizationForStream)(CFReadStreamRef, CFStringRef proxyAuthorizationString);
extern CFHTTPMessageRef (*wkCopyCONNECTProxyResponse)(CFReadStreamRef, CFURLRef responseURL);
extern BOOL (*wkIsLatchingWheelEvent)(NSEvent *);

#ifndef BUILDING_ON_TIGER
extern void (*wkGetGlyphsForCharacters)(CGFontRef, const UniChar[], CGGlyph[], size_t);
#else
#define GLYPH_VECTOR_SIZE (50 * 32)

extern void (*wkClearGlyphVector)(void* glyphs);
extern OSStatus (*wkConvertCharToGlyphs)(void* styleGroup, const UniChar*, unsigned numCharacters, void* glyphs);
extern CFStringRef (*wkCopyFullFontName)(CGFontRef font);
extern OSStatus (*wkGetATSStyleGroup)(ATSUStyle, void** styleGroup);
extern CGFontRef (*wkGetCGFontFromNSFont)(NSFont*);
extern void (*wkGetFontMetrics)(CGFontRef, int* ascent, int* descent, int* lineGap, unsigned* unitsPerEm);
extern ATSLayoutRecord* (*wkGetGlyphVectorFirstRecord)(void* glyphVector);
extern void* wkGetGlyphsForCharacters;
extern int (*wkGetGlyphVectorNumGlyphs)(void* glyphVector);
extern size_t (*wkGetGlyphVectorRecordSize)(void* glyphVector);
extern OSStatus (*wkInitializeGlyphVector)(int count, void* glyphs);
extern void (*wkReleaseStyleGroup)(void* group);
extern BOOL (*wkSupportsMultipartXMixedReplace)(NSMutableURLRequest *);
#endif

extern BOOL (*wkUseSharedMediaUI)();

#if !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD) && !defined(BUILDING_ON_SNOW_LEOPARD)
extern NSMutableArray *(*wkNoteOpenPanelFiles)(NSArray *);
#else
extern void* wkNoteOpenPanelFiles;
#endif

#ifdef __cplusplus
}
#endif

#endif
