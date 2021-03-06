/*
 ** Copyright 2006, The Android Open Source Project
 **
 ** Licensed under the Apache License, Version 2.0 (the "License"); 
 ** you may not use this file except in compliance with the License. 
 ** You may obtain a copy of the License at 
 **
 **     http://www.apache.org/licenses/LICENSE-2.0 
 **
 ** Unless required by applicable law or agreed to in writing, software 
 ** distributed under the License is distributed on an "AS IS" BASIS, 
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
 ** See the License for the specific language governing permissions and 
 ** limitations under the License.
*/

#include <carbon/carbon.h>
#include "SkFontHost.h"
#include "SkDescriptor.h"
#include "SkPoint.h"

// Give 1MB font cache budget
#define FONT_CACHE_MEMORY_BUDGET    (1024 * 1024)

const char* gDefaultfont = "Arial"; // hard code for now
static SkMutex      gFTMutex;

static inline SkPoint F32PtToSkPoint(const Float32Point p) {
    SkPoint sp = { SkFloatToScalar(p.x), SkFloatToScalar(p.y) };
    return sp;
}

static inline uint32_t _rotl(uint32_t v, uint32_t r) {
    return (v << r | v >> (32 - r));
}

// This will generate a unique ID based on the fontname + fontstyle
// and also used by upper layer
uint32_t FontFaceChecksum(const char *name,SkTypeface::Style style)
{
    if (!name) return style;
    
    char* q = (char*)name;

    // From "Performance in Practice of String Hashing Functions"
    // Ramakrishna & Zobel
    const uint32_t L = 5;
    const uint32_t R = 2;
     
    uint32_t h = 0x12345678;
    while (*q) {
        uint32_t ql = tolower(*q);
        h ^= ((h << L) + (h >> R) + ql);
        q ++;
    }

    // add style
    h = _rotl(h, 3) ^ style;

    return h;
}

#pragma mark -
struct SkFaceRec {
    SkFaceRec*      fNext;
    uint32_t        fRefCnt;
    ATSUFontID      fFontID;
    ATSUStyle       fStyle;

    SkFaceRec() : fRefCnt(0), fFontID(0), fStyle(NULL) {};

    ~SkFaceRec() {
        if (fStyle) {
            ::ATSUDisposeStyle(fStyle);
            fStyle = NULL;
        }
    }

    uint32_t ref() {
        return ++fRefCnt;
    }
};

// Font Face list
static SkFaceRec*   gFaceRecHead = NULL;

static SkFaceRec* find_ft_face(const ATSUFontID fontID) {
    SkFaceRec* rec = gFaceRecHead;
    while (rec) {
        if (rec->fFontID == fontID) {
            return rec;
        }
        rec = rec->fNext;
    }

    return NULL;
}

static SkFaceRec* insert_ft_face(const ATSUFontID afontID, const ATSUStyle atsuStyle) {
    SkFaceRec* rec = find_ft_face(afontID);
    if (rec) {
        return rec;  // found?
    }

    rec = SkNEW(SkFaceRec);
    rec->fFontID = afontID;
    rec->fStyle = atsuStyle;
    rec->fNext = gFaceRecHead;
    gFaceRecHead = rec;

    return rec;
}

static void unref_ft_face(const ATSUFontID fontID) {

    SkFaceRec* rec = gFaceRecHead;
    SkFaceRec* prev = NULL;
    while (rec) {
        SkFaceRec* next = rec->fNext;
        if (rec->fFontID == fontID) {
            if (--rec->fRefCnt == 0) {
                if (prev)
                    prev->fNext = next;
                else
                    gFaceRecHead = next;

                SkDELETE(rec);
            }
            return;
        }
        prev = rec;
        rec = next;
    }
    SkASSERT("shouldn't get here, face not in list");
}

#pragma mark -

// have to do this because SkTypeface::SkTypeface() is protected
class SkTypeface_Mac : public SkTypeface {
public:
    SkTypeface_Mac(SkTypeface::Style style, uint32_t id) : SkTypeface(style, id) {}

    ~SkTypeface_Mac() {}
};

#pragma mark -

static SkTypeface* CreateTypeface_(const char *name, const SkTypeface::Style style) {

    OSStatus err;
    ATSUStyle atsuStyle;
    ::ATSUCreateStyle(&atsuStyle);
    if (name != NULL) {
        static const ATSUAttributeTag fontTag = kATSUFontTag;
        static const ByteCount fontTagSize =  sizeof(ATSUFontID);
    
        ATSUFontID fontID = 0;
#if 1
        err = ::ATSUFindFontFromName(
                name,strlen(name),kFontNoNameCode,   /*  instead of regular, kFontFamilyName returns bold and/or italic sometimes, but why this works?? */
                kFontMacintoshPlatform,kFontNoScriptCode,kFontNoLanguageCode,&fontID);
#else    
        CFStringRef cfontName = CFStringCreateWithCString(NULL, name, kCFStringEncodingASCII); 
        ATSFontRef fontRef = ::ATSFontFindFromName(cfontName,kATSOptionFlagsDefault);
        fontID = ::FMGetFontFromATSFontRef(fontRef);
        CFRelease(cfontName);
#endif    
        if (0 != fontID) {
            const ATSUAttributeValuePtr values[] = { &fontID };
            err = ::ATSUSetAttributes(atsuStyle,1,&fontTag,&fontTagSize,values);
        }
        else {
        }
    }
    if (style != SkTypeface::kNormal) {
        Boolean fontItalic = ((style & SkTypeface::kItalic) != 0);
        Boolean fontBold = ((style & SkTypeface::kBold) != 0);
        const ATSUAttributeTag tags[2] =        { kATSUQDBoldfaceTag, kATSUQDItalicTag };
        const ATSUAttributeValuePtr values[2] = { &fontBold, &fontItalic };
        const ByteCount sizes[2] =                { sizeof(Boolean), sizeof(Boolean) };
        err = ::ATSUSetAttributes(atsuStyle,2,tags,sizes,values);
    }

    uint32_t cs = FontFaceChecksum(name,style);
    SkTypeface_Mac* ptypeface = new SkTypeface_Mac(style,cs);

    if (NULL == ptypeface) {
        SkASSERT(false);
        return NULL;
    }

    SkFaceRec* rec = insert_ft_face(cs, atsuStyle);
    SkASSERT(rec);
    if (rec) {};  // Avoid "unused variable" compiler warning.

    return ptypeface;
}

static SkTypeface* CreateTypeface_(const SkFaceRec* rec, const SkTypeface::Style style) {

    OSStatus err;
    ATSUStyle atsuStyle;
    err = ::ATSUCreateAndCopyStyle(rec->fStyle, &atsuStyle);

    Boolean fontItalic = ((style & SkTypeface::kItalic) != 0);
    Boolean fontBold = ((style & SkTypeface::kBold) != 0);
    const ATSUAttributeTag tags[2] =        { kATSUQDBoldfaceTag, kATSUQDItalicTag };
    const ATSUAttributeValuePtr values[2] = { &fontBold, &fontItalic };
    const ByteCount sizes[2] =                { sizeof(Boolean), sizeof(Boolean) };
    err = ::ATSUSetAttributes(atsuStyle,2,tags,sizes,values);

    // get old font id and name
    ATSUFontID fontID = 0;
    ByteCount actual = 0;
    err = ::ATSUGetAttribute(rec->fStyle,kATSUFontTag,sizeof(ATSUFontID),&fontID,&actual);

    ByteCount actualLength = 0;
    char *fontname = NULL;
    err = ::ATSUFindFontName(fontID , kFontFamilyName, kFontUnicodePlatform, kFontNoScriptCode,
        kFontNoLanguageCode , 0 , NULL , &actualLength , NULL );
    if ( err == noErr)
    {
        actualLength += 1 ;
        fontname = (char*)malloc( actualLength );
        err = ::ATSUFindFontName(fontID, kFontFamilyName, kFontUnicodePlatform, kFontNoScriptCode,
            kFontNoLanguageCode, actualLength, fontname , NULL, NULL);
    }

    SkTypeface_Mac* ptypeface = NULL;
    if (fontname == NULL) {
        ptypeface = new SkTypeface_Mac(style,rec->fFontID);
        return ptypeface;
    }
    else {
        uint32_t cs = FontFaceChecksum(fontname,style);
        ptypeface = new SkTypeface_Mac(style, cs);

        if (NULL == ptypeface) {
            SkASSERT(false);
            return NULL;
        }

        free(fontname);

        insert_ft_face(cs,atsuStyle);
    }
    return ptypeface;
}

#pragma mark -

class SkScalerContext_Mac : public SkScalerContext {
public:
    SkScalerContext_Mac(const SkDescriptor* desc);
    virtual ~SkScalerContext_Mac();

protected:
    virtual unsigned generateGlyphCount() const;
    virtual uint16_t generateCharToGlyph(SkUnichar uni);
    virtual void generateAdvance(SkGlyph* glyph);
    virtual void generateMetrics(SkGlyph* glyph);
    virtual void generateImage(const SkGlyph& glyph);
    virtual void generatePath(const SkGlyph& glyph, SkPath* path);
    virtual void generateLineHeight(SkPoint* ascent, SkPoint* descent);
    virtual void generateFontMetrics(SkPaint::FontMetrics* mX, SkPaint::FontMetrics* mY);
//    virtual SkDeviceContext getDC() { return NULL; } // not implemented on Mac

private:
    ATSUTextLayout  fLayout;
    ATSUStyle       fStyle;
    CGColorSpaceRef fGrayColorSpace;
    
    static OSStatus MoveTo(const Float32Point *pt, void *cb);
    static OSStatus Line(const Float32Point *pt, void *cb);
    static OSStatus Curve(const Float32Point *pt1, const Float32Point *pt2, const Float32Point *pt3, void *cb);
    static OSStatus Close(void *cb);
};

SkScalerContext_Mac::SkScalerContext_Mac(const SkDescriptor* desc)
    : SkScalerContext(desc), fLayout(0), fStyle(0)
{
    SkAutoMutexAcquire  ac(gFTMutex);
    OSStatus err;
    
    SkFaceRec* rec = find_ft_face(fRec.fFontID);
    if (rec) {
        rec->ref();
        err = ::ATSUCreateAndCopyStyle(rec->fStyle, &fStyle);
    }
    else {
        SkASSERT(false);
        // create a default
        err = ::ATSUCreateStyle(&fStyle);
    }

    uint32_t size = SkScalarFloor(fRec.fTextSize);
    Fixed fixedSize = IntToFixed(size);
    static const ATSUAttributeTag sizeTag = kATSUSizeTag;
    static const ByteCount sizeTagSize = sizeof(Fixed);
    const ATSUAttributeValuePtr values[] = { &fixedSize };
    err = ::ATSUSetAttributes(fStyle,1,&sizeTag,&sizeTagSize,values);

    err = ::ATSUCreateTextLayout(&fLayout);

    fGrayColorSpace = ::CGColorSpaceCreateDeviceGray();
}

SkScalerContext_Mac::~SkScalerContext_Mac()
{
    ::CGColorSpaceRelease(fGrayColorSpace);

    unref_ft_face(fRec.fFontID);

    ::ATSUDisposeTextLayout(fLayout);
    ::ATSUDisposeStyle(fStyle);
}

unsigned SkScalerContext_Mac::generateGlyphCount() const
{
    return 0xFFFF;
}

uint16_t SkScalerContext_Mac::generateCharToGlyph(SkUnichar uni)
{
    SkAutoMutexAcquire  ac(gFTMutex);
    
    OSStatus err;
    UniChar achar = uni;
    err = ::ATSUSetTextPointerLocation(fLayout,&achar,0,1,1);
    err = ::ATSUSetRunStyle(fLayout,fStyle,kATSUFromTextBeginning,kATSUToTextEnd);
        
    ATSLayoutRecord *layoutPtr;
    ItemCount count;
    ATSGlyphRef glyph;
    
    err = ::ATSUDirectGetLayoutDataArrayPtrFromTextLayout(fLayout,0,kATSUDirectDataLayoutRecordATSLayoutRecordCurrent,(void**)&layoutPtr,&count);
    glyph = layoutPtr->glyphID;
    ::ATSUDirectReleaseLayoutDataArrayPtr(NULL,kATSUDirectDataLayoutRecordATSLayoutRecordCurrent,(void**)&layoutPtr);
    return glyph;
}

static void set_glyph_metrics_on_error(SkGlyph* glyph) {
    glyph->fRsbDelta = 0;
    glyph->fLsbDelta = 0;
    glyph->fWidth    = 0;
    glyph->fHeight   = 0;
    glyph->fTop      = 0;
    glyph->fLeft     = 0;
    glyph->fAdvanceX = 0;
    glyph->fAdvanceY = 0;
}

void SkScalerContext_Mac::generateAdvance(SkGlyph* glyph) {
    this->generateMetrics(glyph);
}

void SkScalerContext_Mac::generateMetrics(SkGlyph* glyph) {
    GlyphID glyphID = glyph->getGlyphID(fBaseGlyphCount);
    ATSGlyphScreenMetrics metrics;

    OSStatus err = ATSUGlyphGetScreenMetrics(fStyle, 1, &glyphID, 0, true, true,
                                             &metrics);
    if (noErr != err) {
        set_glyph_metrics_on_error(glyph);
    } else {
        glyph->fAdvanceX = SkFloatToFixed(metrics.deviceAdvance.x);
        glyph->fAdvanceY = -SkFloatToFixed(metrics.deviceAdvance.y);
        glyph->fWidth = metrics.width;
        glyph->fHeight = metrics.height;
        glyph->fLeft = sk_float_round2int(metrics.topLeft.x);
        glyph->fTop = -sk_float_round2int(metrics.topLeft.y);
    }
}

void SkScalerContext_Mac::generateFontMetrics(SkPaint::FontMetrics* mx,
                                              SkPaint::FontMetrics* my) {
#if 0
    OSStatus ATSFontGetVerticalMetrics (
                                        ATSFontRef iFont,
                                        ATSOptionFlags iOptions,
                                        ATSFontMetrics *oMetrics
    );
#endif
    //SkASSERT(false);
    if (mx)
        memset(mx, 0, sizeof(SkPaint::FontMetrics));
    if (my)
        memset(my, 0, sizeof(SkPaint::FontMetrics));
    return;
}

void SkScalerContext_Mac::generateImage(const SkGlyph& glyph)
{
    SkAutoMutexAcquire  ac(gFTMutex);
    SkASSERT(fLayout);
    OSStatus err;

    bzero(glyph.fImage, glyph.fHeight * glyph.rowBytes());
    CGContextRef contextRef = ::CGBitmapContextCreate(glyph.fImage,
                                            glyph.fWidth, glyph.fHeight, 8,
                                            glyph.rowBytes(), fGrayColorSpace,
                                            kCGImageAlphaNone);
    if (!contextRef) {
        SkASSERT(false);
        return;
    }

    ::CGContextSetGrayFillColor(contextRef, 1.0, 1.0);
    ::CGContextSetTextDrawingMode(contextRef, kCGTextFill);
    
    ATSUAttributeTag tag = kATSUCGContextTag;
    ByteCount size = sizeof(CGContextRef);
    ATSUAttributeValuePtr value = &contextRef;
    err = ::ATSUSetLayoutControls(fLayout, 1, &tag, &size, &value);
    SkASSERT(!err);
    err = ::ATSUDrawText(fLayout, kATSUFromTextBeginning, kATSUToTextEnd,
                         SkIntToFixed(-glyph.fLeft),
                         SkIntToFixed(glyph.fTop + glyph.fHeight));
    SkASSERT(!err);
    ::CGContextRelease(contextRef);
}

void SkScalerContext_Mac::generatePath(const SkGlyph& glyph, SkPath* path)
{
    SkAutoMutexAcquire  ac(gFTMutex);
    OSStatus err,result;
    
    err = ::ATSUGlyphGetCubicPaths(
            fStyle,glyph.fID,
            &SkScalerContext_Mac::MoveTo,
            &SkScalerContext_Mac::Line,
            &SkScalerContext_Mac::Curve,
            &SkScalerContext_Mac::Close,
            path,&result);
    SkASSERT(err == noErr);
}

void SkScalerContext_Mac::generateLineHeight(SkPoint* ascent, SkPoint* descent)
{
    ATSUTextMeasurement     textAscent, textDescent;
    ByteCount actual = 0;
    OSStatus err = ::ATSUGetAttribute(fStyle,kATSULineAscentTag,sizeof(ATSUTextMeasurement),&textAscent,&actual);
    ascent->set(0,textAscent);
    err = ::ATSUGetAttribute(fStyle,kATSULineDescentTag,sizeof(ATSUTextMeasurement),&textDescent,&actual);
    descent->set(0,textDescent);
}

OSStatus SkScalerContext_Mac::MoveTo(const Float32Point *pt, void *cb)
{
    reinterpret_cast<SkPath*>(cb)->moveTo(F32PtToSkPoint(*pt));
    return noErr;
}

OSStatus SkScalerContext_Mac::Line(const Float32Point *pt, void *cb)
{
    reinterpret_cast<SkPath*>(cb)->lineTo(F32PtToSkPoint(*pt));
    return noErr;
}

OSStatus SkScalerContext_Mac::Curve(const Float32Point *pt1,
                                    const Float32Point *pt2,
                                    const Float32Point *pt3, void *cb)
{
    reinterpret_cast<SkPath*>(cb)->cubicTo(F32PtToSkPoint(*pt1),
                                           F32PtToSkPoint(*pt2),
                                           F32PtToSkPoint(*pt3));
    return noErr;
}

OSStatus SkScalerContext_Mac::Close(void *cb)
{
    reinterpret_cast<SkPath*>(cb)->close();
    return noErr;
}

#pragma mark -

void SkFontHost::Serialize(const SkTypeface* face, SkWStream* stream) {
    SkASSERT(!"SkFontHost::Serialize unimplemented");
}

SkTypeface* SkFontHost::Deserialize(SkStream* stream) {
    SkASSERT(!"SkFontHost::Deserialize unimplemented");
    return NULL;
}

SkTypeface* SkFontHost::CreateTypeface(SkStream* stream) {
    
    //Should not be used on Mac, keep linker happy
    SkASSERT(false);
    return CreateTypeface_(gDefaultfont,SkTypeface::kNormal);
}

SkTypeface* SkFontHost::CreateTypefaceFromFile(const char path[]) {
    // TODO
    return CreateTypeface_(gDefaultfont,SkTypeface::kNormal);
}

SkScalerContext* SkFontHost::CreateScalerContext(const SkDescriptor* desc)
{
    return new SkScalerContext_Mac(desc);
}

SkScalerContext* SkFontHost::CreateFallbackScalerContext(const SkScalerContext::Rec& rec)
{
    SkAutoDescriptor    ad(sizeof(rec) + sizeof(gDefaultfont) + SkDescriptor::ComputeOverhead(2));
    SkDescriptor*       desc = ad.getDesc();

    desc->init();
    SkScalerContext::Rec* newRec =
        (SkScalerContext::Rec*)desc->addEntry(kRec_SkDescriptorTag, sizeof(rec), &rec);

    CreateTypeface_(gDefaultfont,SkTypeface::kNormal);
    newRec->fFontID = FontFaceChecksum(gDefaultfont,SkTypeface::kNormal);
    desc->computeChecksum();

    return SkFontHost::CreateScalerContext(desc);
}


    /** Return the closest matching typeface given either an existing family
        (specified by a typeface in that family) or by a familyName, and a
        requested style.
        1) If familyFace is null, use famillyName.
        2) If famillyName is null, use familyFace.
        3) If both are null, return the default font that best matches style
        This MUST not return NULL.
    */

SkTypeface* SkFontHost::FindTypeface(const SkTypeface* familyFace, const char familyName[], SkTypeface::Style style) {
    
    SkAutoMutexAcquire  ac(gFTMutex);
    
    // clip to legal style bits
    style = (SkTypeface::Style)(style & SkTypeface::kBoldItalic);
    
    SkTypeface* tf = NULL;
    
    if (NULL == familyFace && NULL == familyName) {
        tf = CreateTypeface_(gDefaultfont,style);
    }
    else {        
        if (NULL != familyFace) {
            uint32_t id = familyFace->uniqueID();
            SkFaceRec* rec = find_ft_face(id);
            if (!rec) {
                SkASSERT(false);
                tf = CreateTypeface_(gDefaultfont,style);
            }
            else {
                tf = CreateTypeface_(rec,style);
            }
        }
        else {
            tf = CreateTypeface_(familyName,style);
        }
    }

    if (NULL == tf) {
        tf = CreateTypeface_(gDefaultfont,style);
    }
    return tf;

}

size_t SkFontHost::ShouldPurgeFontCache(size_t sizeAllocatedSoFar) {
    if (sizeAllocatedSoFar > FONT_CACHE_MEMORY_BUDGET)
        return sizeAllocatedSoFar - FONT_CACHE_MEMORY_BUDGET;
    else
        return 0;   // nothing to do
}

int SkFontHost::ComputeGammaFlag(const SkPaint& paint) {
    return 0;
}

void SkFontHost::GetGammaTables(const uint8_t* tables[2]) {
    tables[0] = NULL;   // black gamma (e.g. exp=1.4)
    tables[1] = NULL;   // white gamma (e.g. exp= 1/1.4)
}

