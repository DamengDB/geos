/**********************************************************************
 *
 * GEOS - Geometry Engine Open Source
 * http://geos.osgeo.org
 *
 * Copyright (C) 2009-2011 Sandro Santilli <strk@kbt.io>
 * Copyright (C) 2008-2010 Safe Software Inc.
 * Copyright (C) 2005-2007 Refractions Research Inc.
 * Copyright (C) 2001-2002 Vivid Solutions Inc.
 *
 * This is free software; you can redistribute and/or modify it under
 * the terms of the GNU Lesser General Public Licence as published
 * by the Free Software Foundation.
 * See the COPYING file for more information.
 *
 **********************************************************************
 *
 * Last port: operation/buffer/BufferBuilder.java 149b38907 (JTS-1.12)
 *
 **********************************************************************/

#include <geos/geom/GeometryFactory.h>
#include <geos/geom/Location.h>
#include <geos/geom/Geometry.h>
#include <geos/geom/Polygon.h>
#include <geos/geom/GeometryCollection.h>
#include <geos/geom/LineString.h>
#include <geos/geom/MultiLineString.h>
#include <geos/operation/buffer/BufferBuilder.h>
#include <geos/operation/buffer/OffsetCurveBuilder.h>
#include <geos/operation/buffer/BufferCurveSetBuilder.h>
#include <geos/operation/buffer/BufferSubgraph.h>
#include <geos/operation/buffer/SubgraphDepthLocater.h>
#include <geos/operation/overlayng/OverlayNG.h>
#include <geos/operation/overlay/snap/SnapOverlayOp.h>
#include <geos/operation/buffer/PolygonBuilder.h>
#include <geos/operation/buffer/BufferNodeFactory.h>
#include <geos/operation/polygonize/Polygonizer.h>
#include <geos/operation/union/UnaryUnionOp.h>
#include <geos/operation/valid/RepeatedPointRemover.h>
#include <geos/operation/linemerge/LineMerger.h>
#include <geos/algorithm/LineIntersector.h>
#include <geos/noding/IntersectionAdder.h>
#include <geos/noding/SegmentString.h>
#include <geos/noding/MCIndexNoder.h>
#include <geos/noding/NodedSegmentString.h>
#include <geos/geom/Position.h>
#include <geos/geomgraph/PlanarGraph.h>
#include <geos/geomgraph/Label.h>
#include <geos/geomgraph/Node.h>
#include <geos/geomgraph/Edge.h>
#include <geos/util/GEOSException.h>
#include <geos/io/WKTWriter.h> // for debugging
#include <geos/util/IllegalArgumentException.h>
#include <geos/profiler.h>
#include <geos/util/Interrupt.h>

#include <cassert>
#include <vector>
#include <iomanip>
#include <algorithm>
#include <iostream>

// Debug single sided buffer
//#define GEOS_DEBUG_SSB 1

#ifndef GEOS_DEBUG
#define GEOS_DEBUG 0
#endif

#ifndef JTS_DEBUG
#define JTS_DEBUG 0
#endif

//
using namespace geos::geom;
using namespace geos::geomgraph;
using namespace geos::noding;
using namespace geos::algorithm;
using namespace geos::operation::linemerge;

namespace {

// Debug routine
template <class Iterator>
std::unique_ptr<Geometry>
convertSegStrings(const GeometryFactory* fact, Iterator it, Iterator et)
{
    std::vector<std::unique_ptr<Geometry>> lines;
    while(it != et) {
        const SegmentString* ss = *it;
        auto line = fact->createLineString(ss->getCoordinates()->clone());
        lines.push_back(std::move(line));
        ++it;
    }
    return fact->buildGeometry(lines.begin(), lines.end());
}

}

namespace geos {
namespace operation { // geos.operation
namespace buffer { // geos.operation.buffer

#if PROFILE
static Profiler* profiler = Profiler::instance();
#endif

int
BufferBuilder::depthDelta(const Label& label)
{
    Location lLoc = label.getLocation(0, Position::LEFT);
    Location rLoc = label.getLocation(0, Position::RIGHT);
    if(lLoc == Location::INTERIOR && rLoc == Location::EXTERIOR) {
        return 1;
    }
    else if(lLoc == Location::EXTERIOR && rLoc == Location::INTERIOR) {
        return -1;
    }
    return 0;
}

BufferBuilder::~BufferBuilder()
{
    delete li; // could be NULL
    delete intersectionAdder;
}

/*public*/
std::unique_ptr<Geometry>
BufferBuilder::bufferLineSingleSided(const Geometry* g, double distance,
                                     bool leftSide)
{
    // Returns the line used to create a single-sided buffer.
    // Input requirement: Must be a LineString.
    const LineString* l = dynamic_cast< const LineString* >(g);
    if(!l) {
        throw util::IllegalArgumentException("BufferBuilder::bufferLineSingleSided only accept linestrings");
    }

    // Nothing to do for a distance of zero
    if(distance == 0) {
        return g->clone();
    }

    // Get geometry factory and precision model.
    const PrecisionModel* precisionModel = workingPrecisionModel;
    if(!precisionModel) {
        precisionModel = l->getPrecisionModel();
    }

    assert(precisionModel);
    assert(l);

    geomFact = l->getFactory();

    // First, generate the two-sided buffer using a butt-cap.
    BufferParameters modParams = bufParams;
    modParams.setEndCapStyle(BufferParameters::CAP_FLAT);
    modParams.setSingleSided(false); // ignore parameter for areal-only geometries


    // This is a (temp?) hack to workaround the fact that
    // BufferBuilder BufferParameters are immutable after
    // construction, while we want to force the end cap
    // style to FLAT for single-sided buffering
    BufferBuilder tmpBB(modParams);
    std::unique_ptr<Geometry> buf(tmpBB.buffer(l, distance));

    // Create MultiLineStrings from this polygon.
    std::unique_ptr<Geometry> bufLineString(buf->getBoundary());

#ifdef GEOS_DEBUG_SSB
    std::cerr << "input|" << *l << std::endl;
    std::cerr << "buffer|" << *bufLineString << std::endl;
#endif

    // Then, get the raw (i.e. unnoded) single sided offset curve.
    OffsetCurveBuilder curveBuilder(precisionModel, modParams);
    std::vector< CoordinateSequence* > lineList;

    {
        std::unique_ptr< CoordinateSequence > coords(g->getCoordinates());
        curveBuilder.getSingleSidedLineCurve(coords.get(), distance,
                                             lineList, leftSide, !leftSide);
        coords.reset();
    }

    // Create a SegmentString from these coordinates.
    SegmentString::NonConstVect curveList;
    for(unsigned int i = 0; i < lineList.size(); ++i) {
        CoordinateSequence* seq = lineList[i];

        // SegmentString takes ownership of CoordinateSequence
        SegmentString* ss = new NodedSegmentString(seq, seq->hasZ(), seq->hasM(), nullptr);
        curveList.push_back(ss);
    }
    lineList.clear();


    // Node these SegmentStrings.
    Noder* noder = getNoder(precisionModel);
    noder->computeNodes(&curveList);

    SegmentString::NonConstVect* nodedEdges = noder->getNodedSubstrings();

    // Create a geometry out of the noded substrings.
    std::vector<std::unique_ptr<Geometry>> singleSidedNodedEdges;
    singleSidedNodedEdges.reserve(nodedEdges->size());
    for(std::size_t i = 0, n = nodedEdges->size(); i < n; ++i) {
        SegmentString* ss = (*nodedEdges)[i];

        auto tmp = geomFact->createLineString(ss->getCoordinates()->clone());
        delete ss;

        singleSidedNodedEdges.push_back(std::move(tmp));
    }

    delete nodedEdges;

    for(std::size_t i = 0, n = curveList.size(); i < n; ++i) {
        delete curveList[i];
    }
    curveList.clear();

    auto singleSided = geomFact->createMultiLineString(std::move(singleSidedNodedEdges));

#ifdef GEOS_DEBUG_SSB
    std::cerr << "edges|" << *singleSided << std::endl;
#endif

    // Use the boolean operation intersect to obtain the line segments lying
    // on both the butt-cap buffer and this multi-line.
    //Geometry* intersectedLines = singleSided->intersection( bufLineString );
    // NOTE: we use Snapped overlay because the actual buffer boundary might
    //       diverge from original offset curves due to the addition of
    //       intersections with caps and joins curves
    using geos::operation::overlay::snap::SnapOverlayOp;
    std::unique_ptr<Geometry> intersectedLines = SnapOverlayOp::overlayOp(*singleSided, *bufLineString,
            overlayng::OverlayNG::INTERSECTION);

#ifdef GEOS_DEBUG_SSB
    std::cerr << "intersection" << "|" << *intersectedLines << std::endl;
#endif

    // Merge result lines together.
    LineMerger lineMerge;
    lineMerge.add(intersectedLines.get());
    auto mergedLines = lineMerge.getMergedLineStrings();

    // Convert the result into a vector of geometries
    std::vector<std::unique_ptr<Geometry>> mergedLinesGeom;
    const Coordinate& startPoint = l->getCoordinatesRO()->front();
    const Coordinate& endPoint = l->getCoordinatesRO()->back();
    while(!mergedLines.empty()) {
        // Remove end points if they are a part of the original line to be
        // buffered.
        CoordinateSequence::Ptr coords(mergedLines.back()->getCoordinates());
        if(nullptr != coords) {
            // Use 98% of the buffer width as the point-distance requirement - this
            // is to ensure that the point that is "distance" +/- epsilon is not
            // included.
            //
            // Let's try and estimate a more accurate bound instead of just assuming
            // 98%. With 98%, the epsilon grows as the buffer distance grows,
            // so that at large distance, artifacts may skip through this filter
            // Let the length of the line play a factor in the distance, which is still
            // going to be bounded by 98%. Take 10% of the length of the line  from the buffer distance
            // to try and minimize any artifacts.
            const double ptDistAllowance = std::max(distance - l->getLength() * 0.1, distance * 0.98);
            // Use 102% of the buffer width as the line-length requirement - this
            // is to ensure that line segments that is length "distance" +/-
            // epsilon is removed.
            const double segLengthAllowance = 1.02 * distance;

            std::size_t front = 0;
            std::size_t back = coords->size() - 1;
            std::size_t sz = back - front + 1;

            // Clean up the front of the list.
            // Loop until the line's end is not inside the buffer width from
            // the startPoint.
            while (sz > 1 && coords->getAt(front).distance(startPoint) < ptDistAllowance) {
                // Record the end segment length.
                double segLength = coords->getAt(front).distance(coords->getAt(front + 1));

                // Stop looping if there are no more points, or if the segment
                // length is larger than the buffer width.
                if(segLength > segLengthAllowance) {
                    break;
                }

                // If the first point is less than buffer width away from the
                // reference point, then delete the point.
                front++;
                sz--;
            }
            while(sz > 1 && coords->getAt(front).distance(endPoint) < ptDistAllowance) {
                double segLength = coords->getAt(front).distance(coords->getAt(front + 1));
                if(segLength > segLengthAllowance) {
                    break;
                }
                front++;
                sz--;
            }
            // Clean up the back of the list.
            while(sz > 1 && coords->getAt(back).distance(startPoint) < ptDistAllowance) {
                double segLength = coords->getAt(back).distance(coords->getAt(back - 1));

                if(segLength > segLengthAllowance) {
                    break;
                }
                back--;
                sz--;
            }
            while(sz > 1 && coords->getAt(back).distance(endPoint) < ptDistAllowance) {
                double segLength = coords->getAt(back).distance(coords->getAt(back - 1));

                if(segLength > segLengthAllowance) {
                    break;
                }
                back--;
                sz--;
            }

            if (sz > 1) {
                if (sz < coords->size()) {
                    // Points were removed; make a new CoordinateSequence
                    auto newSeq = detail::make_unique<CoordinateSequence>(sz, coords->getDimension());

                    for (std::size_t i = 0; i < sz; i++) {
                        newSeq->setAt(coords->getAt(i + front), i);
                    }

                    coords = std::move(newSeq);
                }

                // Add the coordinates to the resultant line string.
                mergedLinesGeom.push_back(geomFact->createLineString(std::move(coords)));
            }
        }

        mergedLines.pop_back();
    }

    // Clean up.
    if(noder != workingNoder) {
        delete noder;
    }
    buf.reset();
    singleSided.reset();
    intersectedLines.reset();

    if(mergedLinesGeom.size() > 1) {
        return geomFact->createMultiLineString(std::move(mergedLinesGeom));
    }
    else if(mergedLinesGeom.size() == 1) {
        std::unique_ptr<Geometry> single = std::move(mergedLinesGeom[0]);
        return single;
    }
    else {
        return geomFact->createLineString();
    }
}

/*public*/
std::unique_ptr<Geometry>
BufferBuilder::buffer(const Geometry* g, double distance)
// throw(GEOSException *)
{
    // Single-sided buffer only works on single geometries
    // so we'll need to do it individually and then union
    // the result
    if ( bufParams.isSingleSided() && g->getNumGeometries() > 1 )
    {
        std::vector< std::unique_ptr<Geometry> > geoms_to_delete;
        for ( size_t i=0, n=g->getNumGeometries(); i<n; ++i )
        {
            // BufferBuilder class cannot be reused, so
            // we create a new one for each subgeom
            BufferBuilder subbuilder(bufParams);
            const Geometry *subgeom = g->getGeometryN(i);
            std::unique_ptr<Geometry> subbuf = subbuilder.buffer(subgeom, distance);
            geoms_to_delete.push_back( std::move(subbuf) );
        }
        std::vector< Geometry* > geoms;
        for ( size_t i=0, n=geoms_to_delete.size(); i<n; ++i )
            geoms.push_back( geoms_to_delete[i].get() );
        return operation::geounion::UnaryUnionOp::Union(geoms);
    }

    const PrecisionModel* precisionModel = workingPrecisionModel;
    if(precisionModel == nullptr) {
        precisionModel = g->getPrecisionModel();
    }

    assert(precisionModel);
    assert(g);

    // factory must be the same as the one used by the input
    geomFact = g->getFactory();

    {
        // This scope is here to force release of resources owned by
        // BufferCurveSetBuilder when we're doing with it
        BufferCurveSetBuilder curveSetBuilder(*g, distance, precisionModel, bufParams);
        curveSetBuilder.setInvertOrientation(isInvertOrientation);

        GEOS_CHECK_FOR_INTERRUPTS();

        std::vector<SegmentString*>& bufferSegStrList = curveSetBuilder.getCurves();

#if GEOS_DEBUG
        std::cerr << "BufferCurveSetBuilder got " << bufferSegStrList.size()
                  << " curves" << std::endl;
#endif
        // short-circuit test
        if(bufferSegStrList.empty()) {
            return createEmptyResultGeometry();
        }

#if GEOS_DEBUG
        std::cerr << "BufferBuilder::buffer computing NodedEdges" << std::endl;
#endif

        computeNodedEdges(bufferSegStrList, precisionModel);

        GEOS_CHECK_FOR_INTERRUPTS();

    } // bufferSegStrList and contents are released here

#if GEOS_DEBUG > 1
    std::cerr << std::endl << edgeList << std::endl;
#endif

    std::unique_ptr<Geometry> resultGeom(nullptr);
    std::vector<std::unique_ptr<Geometry>> resultPolyList;
    std::vector<BufferSubgraph*> subgraphList;

    try {
        PlanarGraph graph(BufferNodeFactory::instance());
        graph.addEdges(edgeList.getEdges());

        GEOS_CHECK_FOR_INTERRUPTS();

        createSubgraphs(&graph, subgraphList);

#if GEOS_DEBUG
        std::cerr << "Created " << subgraphList.size() << " subgraphs" << std::endl;
#if GEOS_DEBUG > 1
        for(std::size_t i = 0, n = subgraphList.size(); i < n; i++) {
            std::cerr << std::setprecision(10) << *(subgraphList[i]) << std::endl;
        }
#endif
#endif

        GEOS_CHECK_FOR_INTERRUPTS();

        {
            // scope for earlier PolygonBuilder cleanup
            PolygonBuilder polyBuilder(geomFact);
            buildSubgraphs(subgraphList, polyBuilder);

            resultPolyList = polyBuilder.getPolygons();
        }

        // Get rid of the subgraphs, shouldn't be needed anymore
        for(std::size_t i = 0, n = subgraphList.size(); i < n; i++) {
            delete subgraphList[i];
        }
        subgraphList.clear();

#if GEOS_DEBUG
        std::cerr << "PolygonBuilder got " << resultPolyList.size()
                  << " polygons" << std::endl;
#if GEOS_DEBUG > 1
        for(std::size_t i = 0, n = resultPolyList.size(); i < n; i++) {
            std::cerr << resultPolyList[i]->toString() << std::endl;
        }
#endif
#endif

        // just in case ...
        if(resultPolyList.empty()) {
            return createEmptyResultGeometry();
        }

        // resultPolyList ownership transferred here
        resultGeom = geomFact->buildGeometry(std::move(resultPolyList));

    }
    catch(const util::GEOSException& /* exc */) {

        // In case they're still around
        for(std::size_t i = 0, n = subgraphList.size(); i < n; i++) {
            delete subgraphList[i];
        }
        subgraphList.clear();

        throw;
    }

    // Cleanup single-sided buffer artifacts, if needed
    if ( bufParams.isSingleSided() )
    {

        // Get linework of input geom
        const Geometry *inputLineString = g;
        std::unique_ptr<Geometry> inputPolygonBoundary;
        if ( g->getDimension() > 1 )
        {
            inputPolygonBoundary = g->getBoundary();
            inputLineString = inputPolygonBoundary.get();
        }

#if GEOS_DEBUG
        std::cerr << "Input linework: " << *inputLineString << std::endl;
#endif

        // Get linework of buffer geom
        std::unique_ptr<Geometry> bufferBoundary = resultGeom->getBoundary();

#if GEOS_DEBUG
        std::cerr << "Buffer boundary: " << *bufferBoundary << std::endl;
#endif

        // Node all linework
        using operation::overlayng::OverlayNG;
        std::unique_ptr<Geometry> nodedLinework = OverlayNG::overlay(inputLineString, bufferBoundary.get(), OverlayNG::UNION);

#if GEOS_DEBUG
        std::cerr << "Noded linework: " << *nodedLinework << std::endl;
#endif

        using operation::polygonize::Polygonizer;
        Polygonizer plgnzr;
        plgnzr.add(nodedLinework.get());
        std::vector<std::unique_ptr<geom::Polygon>> polys = plgnzr.getPolygons();

#if GEOS_DEBUG
        std::cerr << "Polygonization of noded linework returned: " << polys.size() << " polygons" << std::endl;
#endif

        if ( polys.size() > 1 )
        {
            // Only keep larger polygon
            std::vector<std::unique_ptr<geom::Polygon>>::iterator
                it,
                itEnd = polys.end(),
                biggestPolygonIterator = itEnd;
            double maxArea = 0;
            for ( it = polys.begin(); it != itEnd; ++it )
            {
                double area = (*it)->getArea();
                if ( area > maxArea )
                {
                    biggestPolygonIterator = it;
                    maxArea = area;
                }
            }

            if ( biggestPolygonIterator != itEnd ) {
                Geometry *gg = (*biggestPolygonIterator).release();
                return std::unique_ptr<Geometry>( gg );
            } // else there were no polygons formed...
        }

    }

    return resultGeom;
}

/*private*/
Noder*
BufferBuilder::getNoder(const PrecisionModel* pm)
{
    // this doesn't change workingNoder precisionModel!
    if(workingNoder != nullptr) {
        return workingNoder;
    }

    // otherwise use a fast (but non-robust) noder

    if(li) {  // reuse existing IntersectionAdder and LineIntersector
        li->setPrecisionModel(pm);
        assert(intersectionAdder != nullptr);
    }
    else {
        li = new LineIntersector(pm);
        intersectionAdder = new IntersectionAdder(*li);
    }

    MCIndexNoder* noder = new MCIndexNoder(intersectionAdder);

#if 0
    /* CoordinateArraySequence.cpp:84:
     * virtual const geos::Coordinate& geos::CoordinateArraySequence::getAt(std::size_t) const:
     * Assertion `pos<vect->size()' failed.
     */

    Noder* noder = new IteratedNoder(pm);

    Noder noder = new MCIndexSnapRounder(pm);
    Noder noder = new ScaledNoder(
        new MCIndexSnapRounder(new PrecisionModel(1.0)),
        pm.getScale());
#endif

    return noder;

}

/* private */
void
BufferBuilder::computeNodedEdges(SegmentString::NonConstVect& bufferSegStrList,
                                 const PrecisionModel* precisionModel) // throw(GEOSException)
{
    Noder* noder = getNoder(precisionModel);

#if JTS_DEBUG
    geos::io::WKTWriter wktWriter;
    std::cerr << "before noding: "
              << wktWriter.write(
                  convertSegStrings(geomFact, bufferSegStrList.begin(),
                                    bufferSegStrList.end()).get()
              ) << std::endl;
#endif

    noder->computeNodes(&bufferSegStrList);

    SegmentString::NonConstVect* nodedSegStrings = \
            noder->getNodedSubstrings();

#if JTS_DEBUG
    std::cerr << "after noding: "
              << wktWriter.write(
                  convertSegStrings(geomFact, bufferSegStrList.begin(),
                                    bufferSegStrList.end()).get()
              ) << std::endl;
#endif


    for(SegmentString::NonConstVect::iterator
            i = nodedSegStrings->begin(), e = nodedSegStrings->end();
            i != e;
            ++i) {
        SegmentString* segStr = *i;
        const Label* oldLabel = static_cast<const Label*>(segStr->getData());

        auto cs = operation::valid::RepeatedPointRemover::removeRepeatedPoints(segStr->getCoordinates());
        delete segStr;
        if(cs->size() < 2) {
            // don't insert collapsed edges
            // we need to take care of the memory here as cs is a new sequence
            continue;
        }

        // Edge takes ownership of the CoordinateSequence
        Edge* edge = new Edge(cs.release(), *oldLabel);

        // will take care of the Edge ownership
        insertUniqueEdge(edge);
    }

    delete nodedSegStrings;

    if(noder != workingNoder) {
        delete noder;
    }
}

/*private*/
void
BufferBuilder::insertUniqueEdge(Edge* e)
{
    //<FIX> MD 8 Oct 03  speed up identical edge lookup
    // fast lookup
    Edge* existingEdge = edgeList.findEqualEdge(e);
    // If an identical edge already exists, simply update its label
    if(existingEdge != nullptr) {
        Label& existingLabel = existingEdge->getLabel();
        Label labelToMerge = e->getLabel();

        // check if new edge is in reverse direction to existing edge
        // if so, must flip the label before merging it
        if(! existingEdge->isPointwiseEqual(e)) {
            labelToMerge = e->getLabel();
            labelToMerge.flip();
        }

        existingLabel.merge(labelToMerge);

        // compute new depth delta of sum of edges
        int mergeDelta = depthDelta(labelToMerge);
        int existingDelta = existingEdge->getDepthDelta();
        int newDelta = existingDelta + mergeDelta;
        existingEdge->setDepthDelta(newDelta);

        // we have memory release responsibility
        delete e;

    }
    else {     // no matching existing edge was found

        // add this new edge to the list of edges in this graph
        edgeList.add(e);

        e->setDepthDelta(depthDelta(e->getLabel()));
    }
}

bool
BufferSubgraphGT(BufferSubgraph* first, BufferSubgraph* second)
{
    if(first->compareTo(second) > 0) {
        return true;
    }
    else {
        return false;
    }
}

/*private*/
void
BufferBuilder::createSubgraphs(PlanarGraph* graph, std::vector<BufferSubgraph*>& subgraphList)
{
    std::vector<Node*> nodes;
    graph->getNodes(nodes);
    for(std::size_t i = 0, n = nodes.size(); i < n; i++) {
        Node* node = nodes[i];
        if(!node->isVisited()) {
            BufferSubgraph* subgraph = new BufferSubgraph();
            subgraph->create(node);
            subgraphList.push_back(subgraph);
        }
    }

    /*
     * Sort the subgraphs in descending order of their rightmost coordinate
     * This ensures that when the Polygons for the subgraphs are built,
     * subgraphs for shells will have been built before the subgraphs for
     * any holes they contain
     */
    std::sort(subgraphList.begin(), subgraphList.end(), BufferSubgraphGT);
}

/*private*/
void
BufferBuilder::buildSubgraphs(const std::vector<BufferSubgraph*>& subgraphList,
                              PolygonBuilder& polyBuilder)
{

#if GEOS_DEBUG
    std::cerr << __FUNCTION__ << " got " << subgraphList.size() << " subgraphs" << std::endl;
#endif
    std::vector<BufferSubgraph*> processedGraphs;
    for(std::size_t i = 0, n = subgraphList.size(); i < n; i++) {
        BufferSubgraph* subgraph = subgraphList[i];
        Coordinate* p = subgraph->getRightmostCoordinate();
        assert(p);

#if GEOS_DEBUG
        std::cerr << " " << i << ") Subgraph[" << subgraph << "]" << std::endl;
        std::cerr << "  rightmost Coordinate " << *p;
#endif
        SubgraphDepthLocater locater(&processedGraphs);
#if GEOS_DEBUG
        std::cerr << " after SubgraphDepthLocater processedGraphs contain "
                  << processedGraphs.size()
                  << " elements" << std::endl;
#endif
        int outsideDepth = locater.getDepth(*p);
#if GEOS_DEBUG
        std::cerr << " Depth of rightmost coordinate: " << outsideDepth << std::endl;
#endif
        subgraph->computeDepth(outsideDepth);
        subgraph->findResultEdges();
#if GEOS_DEBUG
        std::cerr << " after computeDepth and findResultEdges subgraph contain:" << std::endl
                  << "   " << subgraph->getDirectedEdges()->size() << " DirecteEdges " << std::endl
                  << "   " << subgraph->getNodes()->size() << " Nodes " << std::endl;
#endif
        processedGraphs.push_back(subgraph);
#if GEOS_DEBUG
        std::cerr << " added " << subgraph << " to processedGraphs, new size is "
                  << processedGraphs.size() << std::endl;
#endif
        polyBuilder.add(subgraph->getDirectedEdges(), subgraph->getNodes());
    }
}

/*private*/
std::unique_ptr<geom::Geometry>
BufferBuilder::createEmptyResultGeometry() const
{
    return geomFact->createPolygon();
}

} // namespace geos.operation.buffer
} // namespace geos.operation
} // namespace geos

