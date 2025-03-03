/*  LabelRenderer.cpp
 *  WhirlyGlobeLib
 *
 *  Created by Steve Gifford on 4/11/13.
 *  Copyright 2011-2022 mousebird consulting
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#import "LabelManager.h"
#import "LabelRenderer.h"
#import "WhirlyGeometry.h"
#import "GlobeMath.h"
#import "LabelManager.h"
#import "SharedAttributes.h"
#import "WhirlyKitLog.h"

using namespace Eigen;
using namespace WhirlyKit;

namespace WhirlyKit
{

static LabelJustify parseLabelJustify(const std::string &str, LabelJustify def)
{
    if (str == MaplyLabelJustifyNameMiddle) return WhirlyKitLabelMiddle;
    if (str == MaplyLabelJustifyNameLeft) return WhirlyKitLabelLeft;
    if (str == MaplyLabelJustifyNameRight) return WhirlyKitLabelRight;
    return def;
}

static TextJustify parseTextJustify(const std::string &str, TextJustify def)
{
    if (str == MaplyTextJustifyCenter) return WhirlyKitTextCenter;
    if (str == MaplyTextJustifyLeft) return WhirlyKitTextLeft;
    if (str == MaplyTextJustifyRight) return WhirlyKitTextRight;
    return def;
}

LabelInfo::LabelInfo(bool screenObject) :
    screenObject(screenObject),
    width(screenObject ? 16.0 : 0.001),
    height(screenObject ? 16.0 : 0.001)
{
}

LabelInfo::LabelInfo(const Dictionary &dict, bool screenObject) :
    BaseInfo(dict),
    screenObject(screenObject)
{
    hasTextColor = dict.hasField(MaplyTextColor);
    textColor = dict.getColor(MaplyTextColor, textColor);
    backColor = dict.getColor(MaplyBackgroundColor, backColor);
    width = (float)dict.getDouble(MaplyLabelWidth,0.0);
    height = (float)dict.getDouble(MaplyLabelHeight,screenObject ? 16.0 : 0.001);
    shadowColor = dict.getColor(MaplyShadowColor, shadowColor);
    shadowSize = (float)dict.getDouble(MaplyShadowSize, 0.0);
    outlineSize = (float)dict.getDouble(MaplyTextOutlineSize,0.0);
    outlineColor = dict.getColor(MaplyTextOutlineColor, outlineColor);
    lineHeight = (float)dict.getDouble(MaplyTextLineHeight,0.0);
    labelJustify = parseLabelJustify(dict.getString(MaplyLabelJustifyName), WhirlyKitLabelMiddle);
    textJustify = parseTextJustify(dict.getString(MaplyTextJustify), WhirlyKitTextLeft);
    labelVAlign = WhirlyKitLabelBaseline;//todo
    layoutDebug = dict.getInt(MaplyTextLayoutDebug,false);
    layoutRepeat = dict.getInt(MaplyTextLayoutRepeat,-1);
    layoutSpacing = (float)dict.getDouble(MaplyTextLayoutSpacing,24.0);
    layoutOffset = (float)dict.getDouble(MaplyTextLayoutOffset,0.0);
}


// We use these for labels that have icons
// Don't want to give them their own separate drawable, obviously

LabelRenderer::LabelRenderer(Scene *scene,
                             SceneRenderer *renderer,
                             FontTextureManagerRef fontTexManager,
                             const LabelInfo *labelInfo,
                             SimpleIdentity maskProgID) :
    scene(scene),
    renderer(renderer),
    coordAdapter(scene->getCoordAdapter()),
    fontTexManager(std::move(fontTexManager)),
    labelInfo(labelInfo),
    maskProgID(maskProgID)
{
}

Point3dVector LabelRenderer::convertGeoPtsToModelSpace(const VectorRing &inPts) const
{
    CoordSystemDisplayAdapter *coordAdapt = scene->getCoordAdapter();
    CoordSystem *coordSys = coordAdapt->getCoordSystem();

    Point3dVector outPts;
    outPts.reserve(inPts.size());
    
    for (const auto &pt: inPts)
    {
        const auto localPt = coordSys->geographicToLocal3d(GeoCoord(pt.x(),pt.y()));
        outPts.push_back(coordAdapt->localToDisplay(localPt));
    }
    
    return outPts;
}

void LabelRenderer::render(PlatformThreadInfo *threadInfo,const std::vector<SingleLabel *> &labels,ChangeSet &changes)
{
    render(threadInfo,labels,changes,[](auto){return false;});
}

void LabelRenderer::render(PlatformThreadInfo *threadInfo,
                           const std::vector<SingleLabel *> &labels,
                           ChangeSet &changes,
                           const std::function<bool(PlatformThreadInfo*)>& cancelFn)
{
    const TimeInterval curTime = scene->getCurrentTime();

    for (const auto label : labels)
    {
        const RGBAColor &theBackColor = labelInfo->backColor;
        const RGBAColor &theShadowColor = labelInfo->shadowColor;
        const RGBAColor &theTextColor = (label->infoOverride && label->infoOverride->hasTextColor) ?
                                            label->infoOverride->textColor : labelInfo->textColor;
        const float theShadowSize = labelInfo->shadowSize;

        // We set this if the color is embedded in the "font"
        const bool embeddedColor = labelInfo->outlineSize > 0.0 ||
                                    (label->infoOverride && label->infoOverride->outlineSize > 0.0);

        // Ask the label to build the strings.  There are OS specific things in there
        // We also need the real line height back (because it's in the font)
        float lineHeight = 0.0;
        const auto drawStrs = label->generateDrawableStrings(threadInfo,labelInfo,fontTexManager,
                                                             lineHeight,changes);

        if (cancelFn(threadInfo) || drawStrs.empty())
        {
            return;
        }

        // Calculate total draw and layout MBRs
        Mbr drawMbr, layoutMbr;
        for (const auto &drawStr : drawStrs)
        {
            drawMbr.expand(drawStr->mbr);
            layoutMbr.expand(drawStr->mbr);
        }

        // Override the layout size, but do so from the middle
        if (label->layoutSize.x() >= 0.0 && label->layoutSize.y() >= 0.0)
        {
            const Point2f center = layoutMbr.mid();
            const Point2f layoutSize(label->layoutSize.x(),label->layoutSize.y());
            layoutMbr.ll() = center - layoutSize/2.0;
            layoutMbr.ur() = center + layoutSize/2.0;
        }

        // Set if we're letting the layout engine control placement
        const bool layoutEngine = label->layoutEngine;
        const float layoutImportance = label->layoutImportance;
        const int layoutPlacement = label->layoutPlacement;
        
        ScreenSpaceObject scratchScreenSpaceObject;
        LayoutObject scratchLayoutObject;
        ScreenSpaceObject *screenShape = nullptr;
        LayoutObject *layoutObject = nullptr;

        // Portions of the label that are shared between substrings
        Point2d iconOff(0,0);
        Point2d justifyOff(0,0);
        if (labelInfo->screenObject)
        {
            const float height = drawMbr.ur().y()-drawMbr.ll().y();

            // If we're doing layout, don't justify it
            if (!layoutEngine)
            {
                const auto width = drawMbr.ur().x() - drawMbr.ll().x();
                switch (labelInfo->labelJustify)
                {
                    case WhirlyKitLabelRight:  justifyOff.x() = -width; break;
                    case WhirlyKitLabelMiddle: justifyOff.x() = -width / 2.0; break;
                    default: break;
                }
            }
            
            screenShape = layoutEngine ? &scratchLayoutObject : &scratchScreenSpaceObject;
            layoutObject = layoutEngine ? &scratchLayoutObject : nullptr;

#ifndef __ANDROID__
            // Except we do need to tweak things a little, even for the layout engine
            // Note: But only on iOS because... reasons
            if (labelInfo->labelVAlign == WhirlyKitLabelBaseline)
            {
                const float heightAboveBaseline = drawMbr.ur().y();
                justifyOff.y() += heightAboveBaseline/2.0f;
            }
#else
            if (labelInfo->labelVAlign == WhirlyKitLabelBaseline)
            {
                justifyOff.y() -= drawMbr.ll().y();
            }
#endif

            screenShape->setDrawOrder(labelInfo->drawOrder);
            screenShape->setDrawPriority(labelInfo->drawPriority+1);
            screenShape->setVisibility((float)labelInfo->minVis, (float)labelInfo->maxVis);
            screenShape->setZoomInfo(labelInfo->zoomSlot, labelInfo->minZoomVis, labelInfo->maxZoomVis);
            screenShape->setScaleExp(labelInfo->scaleExp);
            screenShape->setOpacityExp(labelInfo->opacityExp);
            screenShape->setKeepUpright(label->keepUpright);
            if (label->rotation != 0.0)
                screenShape->setRotation(label->rotation);
            if (labelInfo->fadeIn > 0.0)
                screenShape->setFade(curTime+labelInfo->fadeIn, curTime);
            else if (labelInfo->fadeOutTime != 0.0)
            {
                // up<down=fade out
                screenShape->setFade(/*up=*/labelInfo->fadeOutTime, /*down=*/labelInfo->fadeOutTime+labelInfo->fadeOut);
            }
            if (label->isSelectable && label->selectID != EmptyIdentity)
                screenShape->setId(label->selectID);
            screenShape->setWorldLoc(coordAdapter->localToDisplay(coordAdapter->getCoordSystem()->geographicToLocal3d(label->loc)));
            if (label->hasMotion)
                screenShape->setMovingLoc(coordAdapter->localToDisplay(coordAdapter->getCoordSystem()->geographicToLocal3d(label->endLoc)), label->startTime, label->endTime);
            
            // If there's an icon, we need to offset
            const Point2d iconSize = ((label->iconTexture == EmptyIdentity) ? Point2d(0,0) :
                                        ((label->iconSize.x() == 0.0) ? Point2d(height,height) :
                                            label->iconSize.cast<double>()));
            iconOff = iconSize;

            // Compute the outline points if we need them for any of several things
            Point2dVector geomCoords;
            if (layoutEngine || theBackColor.a != 0.0 || label->maskID != EmptyIdentity)
            {
                // Note: This is an arbitrary border around the text
                const double backBorder = (theBackColor.a != 0.0 || label->maskID != EmptyIdentity) ? 4 : 0;
                justifyOff.x() += backBorder;

                const Point2d ll = drawMbr.ll().cast<double>() - Point2d(backBorder,backBorder);
                const Point2d ur = drawMbr.ur().cast<double>() + Point2d(backBorder,backBorder);

                geomCoords.reserve(4);
                geomCoords.emplace_back(ur.x(),ll.y());
                geomCoords.emplace_back(ur.x(),ur.y());
                geomCoords.emplace_back(ll.x(),ur.y());
                geomCoords.emplace_back(ll.x(),ll.y());

                for (auto &p : geomCoords)
                {
                    p += iconOff + justifyOff + label->screenOffset;
                }
            }

            // Throw a rectangle in the background
            ScreenSpaceConvexGeometry backGeom;
            if (theBackColor.a != 0.0 || label->maskID != EmptyIdentity)
            {
                ScreenSpaceConvexGeometry smGeom;
                smGeom.progID = labelInfo->programID;
                smGeom.coords = geomCoords;

                smGeom.texCoords.reserve(4);
                smGeom.texCoords.emplace_back(0,1);
                smGeom.texCoords.emplace_back(0,0);
                smGeom.texCoords.emplace_back(1,0);
                smGeom.texCoords.emplace_back(1,1);

                smGeom.drawPriority = labelInfo->drawPriority;
                smGeom.color = theBackColor;
                if (label->maskID != EmptyIdentity && label->maskRenderTargetID != EmptyIdentity)
                {
                    // Make a copy for masking
                    backGeom = smGeom;
                }
                screenShape->addGeometry(std::move(smGeom));
            }
            
            // Handle the mask rendering if needed
            if (label->maskID != EmptyIdentity && label->maskRenderTargetID != EmptyIdentity)
            {
                // Make a copy of the geometry, but target it to the mask render target
                const auto slot = renderer->getSlotForNameID(a_maskNameID);
                backGeom.vertexAttrs.emplace(a_maskNameID, slot, (int)label->maskID);
                backGeom.renderTargetID = label->maskRenderTargetID;
                backGeom.progID = maskProgID;
                screenShape->addGeometry(std::move(backGeom));
            }

            // If it's being passed to the layout engine, do that as well
            if (layoutEngine)
            {
                // Propagate the unique ID to the layout object
                layoutObject->uniqueID = label->uniqueID;

                // Potentially lay it out along with other objects
                layoutObject->mergeID = label->mergeID;

                // Put together the layout info
                //layoutObject->hint = label->text;
                layoutObject->layoutPts = geomCoords;
                layoutObject->selectPts = geomCoords;
                
                //layoutObj->iconSize = Point2f(iconSize,iconSize);
                layoutObject->importance = layoutImportance;
                layoutObject->acceptablePlacement = layoutPlacement;
                layoutObject->setEnable(labelInfo->enable);
                layoutObject->layoutDebug = labelInfo->layoutDebug;

                // Setup layout points if we have them
                if (!label->layoutShape.empty())
                {
                    layoutObject->layoutShape = convertGeoPtsToModelSpace(label->layoutShape);
                    layoutObject->layoutRepeat = labelInfo->layoutRepeat;
                    layoutObject->layoutOffset = labelInfo->layoutOffset;
                    layoutObject->layoutSpacing = labelInfo->layoutSpacing;
                    layoutObject->layoutWidth = height;
                }
                
                // The shape starts out disabled
                screenShape->setEnable(labelInfo->enable);
                if (labelInfo->startEnable != labelInfo->endEnable)
                {
                    screenShape->setEnableTime(labelInfo->startEnable, labelInfo->endEnable);
                }
                screenShape->setOffset(Point2d(MAXFLOAT,MAXFLOAT));
            }
            else
            {
                screenShape->setEnable(labelInfo->enable);
                if (labelInfo->startEnable != labelInfo->endEnable)
                {
                    screenShape->setEnableTime(labelInfo->startEnable, labelInfo->endEnable);
                }
            }
            
            // Deal with the icon here because we need its geometry
            ScreenSpaceConvexGeometry iconGeom;
            if (label->iconTexture != EmptyIdentity)
            {
                const SubTexture subTex = scene->getSubTexture(label->iconTexture);
                std::vector<TexCoord> texCoord = {
                        { 0.0, 1.0 },
                        { 1.0, 1.0 },
                        { 1.0, 0.0 },
                        { 0.0, 0.0 },
                };
                subTex.processTexCoords(texCoord);
                
                iconGeom.texIDs.push_back(subTex.texId);
                iconGeom.progID = labelInfo->programID;
                const Point2d iconPts[4] = {
                        { 0, 0 },
                        { iconOff.x(), 0 },
                        iconOff,
                        { 0, iconOff.y() },
                };

                for (unsigned int ii=0;ii<4;ii++)
                {
                    iconGeom.coords.push_back(iconPts[ii] + label->screenOffset);
                    iconGeom.texCoords.push_back(texCoord[ii]);
                }
                screenShape->addGeometry(iconGeom);
            }
            
            // Register the main label as selectable
            if (label->isSelectable && !layoutObject)
            {
                // If the label doesn't already have an ID, it needs one
                if (!label->selectID)
                    label->selectID = Identifiable::genId();
                
                RectSelectable2D select2d;
                select2d.center = screenShape->getWorldLoc();
                select2d.enable = labelInfo->enable;
                Mbr wholeMbr = drawMbr;
                wholeMbr.ll() += Point2f(iconOff.x(),iconOff.y()) + Point2f(justifyOff.x(),justifyOff.y());
                wholeMbr.ur() += Point2f(iconOff.x(),iconOff.y()) + Point2f(justifyOff.x(),justifyOff.y());
                // If there's an icon, just expand the whole thing.
                // Note: Not ideal
                if (!iconGeom.coords.empty())
                {
                    for (const auto &c : iconGeom.coords)
                    {
                        wholeMbr.addPoint(c);
                    }
                }
                const Point2f ll = wholeMbr.ll();
                const Point2f ur = wholeMbr.ur();
                double flip = 1.0;
//#ifdef __ANDROID__
//                flip = -1.0;
//#endif
                select2d.pts[0] = Point2f(ll.x()+label->screenOffset.x(),ll.y()+-flip*label->screenOffset.y());
                select2d.pts[1] = Point2f(ll.x()+label->screenOffset.x(),ur.y()+-flip*label->screenOffset.y());
                select2d.pts[2] = Point2f(ur.x()+label->screenOffset.x(),ur.y()+-flip*label->screenOffset.y());
                select2d.pts[3] = Point2f(ur.x()+label->screenOffset.x(),ll.y()+-flip*label->screenOffset.y());
                
                select2d.selectID = label->selectID;
                select2d.minVis = (float)labelInfo->minVis;
                select2d.maxVis = (float)labelInfo->maxVis;
                
                if (label->hasMotion)
                {
                    MovingRectSelectable2D movingSelect2d;
                    (RectSelectable2D &)movingSelect2d = select2d;

                    movingSelect2d.endCenter = screenShape->getEndWorldLoc();
                    movingSelect2d.startTime = screenShape->getStartTime();
                    movingSelect2d.endTime = screenShape->getEndTime();
                    movingSelectables2D.push_back(movingSelect2d);
                }
                else
                {
                    selectables2D.push_back(select2d);
                }
            }
        }

        // Work through the lines
        double offsetY = 0.0;
        for (auto it = drawStrs.rbegin(); it != drawStrs.rend(); ++it)
        {
            const auto &drawStr = *it;
            if (!drawStr)
                continue;
            
            labelRep->drawStrIDs.insert(drawStr->getId());

            if (labelInfo->screenObject)
            {
                Point2d lineOff(0.0,0.0);
                switch (labelInfo->textJustify)
                {
                    case WhirlyKitTextCenter:
                        lineOff.x() = (drawMbr.ur().x()-drawMbr.ll().x() - (drawStr->mbr.ur().x()-drawStr->mbr.ll().x()))/2.0;
                        break;
                    case WhirlyKitTextRight:
                        lineOff.x() = drawMbr.ur().x()-drawMbr.ll().x() - (drawStr->mbr.ur().x()-drawStr->mbr.ll().x());
                        break;
                    default: break;
                }
                
                // Turn the glyph polys into simple geometry
                // We do this in a weird order to stick the shadow underneath
                for (int ss=((theShadowSize > 0.0) ? 0 : 1);ss<2;ss++)
                {
                    const Point2d soff = (ss == 1) ? Point2d(0,0) : Point2d(theShadowSize,theShadowSize);
                    const RGBAColor color = (ss == 1) ? (embeddedColor ? RGBAColor::white() : theTextColor) : theShadowColor;
                    for (const auto &poly : drawStr->glyphPolys)
                    {
                        // Note: Ignoring the desired size in favor of the font size
                        ScreenSpaceConvexGeometry smGeom;
                        smGeom.progID = labelInfo->programID;
                        smGeom.coords.push_back(Point2d(poly.pts[1].x()+label->screenOffset.x(),poly.pts[0].y()+label->screenOffset.y() + offsetY) + soff + iconOff + justifyOff + lineOff);
                        smGeom.texCoords.emplace_back(poly.texCoords[1].u(),poly.texCoords[0].v());
                        
                        smGeom.coords.push_back(Point2d(poly.pts[1].x()+label->screenOffset.x(),poly.pts[1].y()+label->screenOffset.y() + offsetY) + soff + iconOff + justifyOff + lineOff);
                        smGeom.texCoords.emplace_back(poly.texCoords[1].u(),poly.texCoords[1].v());
                        
                        smGeom.coords.push_back(Point2d(poly.pts[0].x()+label->screenOffset.x(),poly.pts[1].y()+label->screenOffset.y() + offsetY) + soff + iconOff + justifyOff + lineOff);
                        smGeom.texCoords.emplace_back(poly.texCoords[0].u(),poly.texCoords[1].y());
                        
                        smGeom.coords.push_back(Point2d(poly.pts[0].x()+label->screenOffset.x(),poly.pts[0].y()+label->screenOffset.y() + offsetY) + soff + iconOff + justifyOff + lineOff);
                        smGeom.texCoords.emplace_back(poly.texCoords[0].u(),poly.texCoords[0].v());
                        
                        smGeom.texIDs.push_back(poly.subTex.texId);
                        smGeom.color = color;
                        poly.subTex.processTexCoords(smGeom.texCoords);
                        screenShape->addGeometry(std::move(smGeom));
                    }
                }
            }

            offsetY += lineHeight;
        }
        
        if (layoutObject)
        {
            if (layoutObjects.empty())
            {
                layoutObjects.reserve(labels.size());
            }
            layoutObjects.emplace_back(std::move(*layoutObject));
        }
        else if (screenShape)
        {
            if (screenObjects.empty())
            {
                screenObjects.reserve(labels.size());
            }
            screenObjects.emplace_back(std::move(*screenShape));
        }

    }
}

}

#include <utility>
