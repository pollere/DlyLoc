/*
 * flowDelay holds per flow information and implements delay variation
 * computation methods
 *
 * Flow record for an active flow with methods to compute delay variation
 * based partially upon work supported by the Department of Energy under
 * Award Number DE-SC0009498 for the years 2013-2016.
 */

/* Copyright (C) 2022 Pollere LLC
 * All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of a BSD-style License. You should have received a 
 *  copy of the License along with this program. 
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software 
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 *  This program is distributed in the hope that it will be useful.
 *  You may contact Pollere LLC at info@pollere.net.
 *
 */

// extended timestamp value to deal with wraps
const int64_t wrapCnt = 0x10000l;
struct tsWrap {
    int64_t offset[2];
    uint32_t last;
};
static inline int64_t extendTS(uint32_t ts, struct tsWrap *tsw) {
    if( (tsw->last & ~ts) >> 31) {
        //timestamp wrapped
        tsw->offset[1] = tsw->offset[0];
        tsw->offset[0] += wrapCnt;
    }
    tsw->last = ts;
    return tsw->offset[ts>>31] + ts;
}

struct tSamp {
    tSamp(double t, int64_t v) {
        tm = t;
        ts = v; //extended value
    }
    double tm;
    int64_t ts;
};
struct pktInfo {
    double tm;          //capture time adjusted by frstTm
    int64_t ts, ecr;    // extended TSval, ECR
    int sz;             //total bytes
    double dv[3];       //delay variations in sec (or negative 1 if can't compute
    std::string IPsrc, IPdst;
};

struct flowDly
{
    explicit flowDly(std::string nm) : _mm{5.0,50}, lhPts{}
    {
        _flowname = std::move(nm);
        twrap.offset[0] = twrap.offset[1] = 0;
        twrap.last = 0;
        ewrap.offset[0] = ewrap.offset[1] = 0;
        ewrap.last = 0;
        _mm.setFirstInterval();  //since use adjusted values, sets start at 0
    };
    ~flowDly() = default;

    std::string _flowname;
    double _lastTm{};     //capture time for last packet
    double _minPP{1e30};   // current min value for capturepoint-to-source-to-CP RTT
    int64_t _minTS{};      // adjusted (-startTS) TSval when current min was computed
    double _minTm{};    // capture time when this min was seen
    double bytesSnt{};  // number of bytes sent through CP toward dst: inbound-to-CP, or return, direction
    bool revFlow{};     //inidcates if a reverse flow has been seen
    flowDly* rfp{};

    /* for segment delay variation */
    movingMin _mm;     //keeps a moving min of the capture time vs TSval points
    int pktCnt{};       //number of packets sent through CP toward dst
    int64_t zeroTS{};     //extended TSval used for reference as the start of the slope
    double zeroTm;      //the capture time of the zeroTS (estimate of time when zero added delay)
    double startTm{};     //time at flow start
    int64_t startTS{};  //TSval at flow start
    tsWrap twrap;
    tsWrap ewrap;
    tSamp lstTS{0,0};   //last unique TS (unadjusted)

    std::vector<tSamp> lhPts; //keep lower hull points including colinear pts
    double spTS{0};     //seconds per TS tick
    double spSet;       //last time set the spTS value
    bool clkSet{};      //true when there is a "clock" for this flow

    /* for computing lower hull */
    double cross(const tSamp O, const tSamp A, const tSamp B)
    { return (A.ts - O.ts) * (B.tm - O.tm) - (A.tm - O.tm) * (B.ts - O.ts); }
    /*
     * find candidate slope of sec per TS tick using lower hull over local minimum points
     */
    bool computeTicks(double tm, int64_t ts) {        
        if(pktCnt && lstTS.ts >= ts) { //use only first time see a TSval
            return clkSet;
        }
        lstTS = {tm,ts};
        tm -= startTm;   //work with adjusted values to get slope
        ts -= startTS;
        // Track the minimum values over 100 tick intervals using 20 tick subintervals (set in movingmin.hpp)
        std::vector<tSamp> lhSegs = lhPts; //keep lh without intermediate colinear pts
        _mm.addSample(tm,ts);
        if(_mm.newInterval(ts)) {
            minSamp p = _mm.intervalMin();   //add this local min to lower hull
            //test against lower hull points
            auto newVal = tSamp{p.first, p.second};
            while(lhPts.size() >= 2 && cross(lhPts[lhPts.size()-2], lhPts[lhPts.size()-1], newVal) < 0.0) {
                lhPts.pop_back();
            }
            lhPts.push_back(newVal);
            while(lhSegs.size() >= 2 && cross(lhSegs[lhSegs.size()-2], lhSegs[lhSegs.size()-1], newVal) <= 0.0) {
                lhSegs.pop_back();
            }
            lhSegs.push_back(newVal);
        } else
             return clkSet; //do nothing until in a new movingmin interval
        // these numbers are somewhat arbitrary
        if(ts < 3*interval || lhPts.size() < 2 || pktCnt < 20) {
            return clkSet;  //wait 3 movingmin intervals before computing
        }

        //find the longest segment in the lower hull (ignoring intermediate colinear pts) and use its end pt as candidate reference zero
        int64_t longestSeg = 0;
        int li = 0;
        for(int i=1; i<lhSegs.size(); i++){
            if(lhSegs[i].ts - lhSegs[i-1].ts >= longestSeg) {
                longestSeg = lhSegs[i].ts - lhSegs[i-1].ts;
                li = i;
            }
        }
        if(lhSegs[li].ts+startTS == zeroTS)  {  //test for same interval
            if(_minTS > zeroTS) {               //test for later min pp
                zeroTS = _minTS;                //move the reference zero
                zeroTm = _minTm;
            }
            return clkSet;                      //don't recompute
        }

        auto m = (lhSegs[li].tm - lhSegs[li-1].tm)/(lhSegs[li].ts - lhSegs[li-1].ts);
        //figure out if it's usable - need to change for us ticks
        double spt = round(m*1000.)/1000.;   //sec per tick rounded to nearest ms
        if(spt == 0.) {
            clkSet = false;
            return clkSet;
        }
        auto skew = fabs(m - spt);       //skew in sec per ts tick
        //skew should be less than limit, conservative is 50 us per sec (0.00005)
        if(skew/spt > 0.005)  {
            clkSet = false;    //in case it was looking okay, switch
            return clkSet;   //can't determine a clock
        }
        spTS =  spt;    //sec per TS tick rounded to nearest ms
        zeroTS = startTS + lhSegs[li].ts;
        zeroTm = startTm + lhSegs[li].tm;
        clkSet = true;
        spSet = tm;
        return clkSet;
    }

    /*
    * Compute delay variations for a packet (in integer microsecs)
     *
     * compute the path dvs for all three paths - do any testing of dv here
     * path seg 0 is from the destination of this packet to the packet's sender
     * path seg 1 is sender of this packet to this dlyloc
     * path seg 2 is the sum of path 0 and path 1
     *
     * If bad source clock, no d0 or d1. If bad dest clock, no d0 or d2
     * Not using ECR extracted clocks, so must be reverse flow to get dest clock
     *
     * Hack to deal with not knowing when there is minimum delay:
     *  hold on to samples until find a recent min (must have sufficient time/#samples to determine
     *  use the sample associated with a min pping
     *
     */
    bool computeDV(pktInfo& pi)
    {
        double srcTm;
        bool setDV = false;
        /*
         * Capture time is equal to time packet was sent + (unknown) "intrinsic delay" + queue delay
         * Assume that the zero point (zeroTm, zeroTS) means queue delay = 0 so that the
         * (capture time of zero point) - (TSval of zero point converted source time set as zero
         * reference) = intrinsic delay
         * We can only get the queue delay (unless we want to guesstimate the intrinsic delay as half pp_min)
         * Estimate time packet was sent as the time passed at the source since the zero point was sent
         * + zero point's capture time - intrinsic delay
         * Then, for pi, its capture time - sent time = queue delay + intrinsic delay
         * Since we don't know intrinsic delay, we just compute queue delay and subtract (ignore) the
         * intrinsic delay on both sides
         */
        if(computeTicks(pi.tm, pi.ts)) {   //check for a usable clock from TSvals
            //adjusted time passed at sourceIP + fudge factor for multiple packets with same TSval
            //estimate of time at source plus the (unknown) min delay
            // has to be for a point captured after zero point
            srcTm = double ((pi.ts - zeroTS) * spTS) + zeroTm; // + (pi.tm - lstTS.tm);
            if(srcTm > pi.tm) {
                srcTm = pi.tm;
            }
            //time at CP - srcTm is any added delay beyond min
            pi.dv[1] = pi.tm - srcTm;
            setDV = true;
        }
        //(since tested here, skip try...catch below)
        if(!revFlow) {
            return setDV;
        }
        if(rfp == nullptr)  return setDV;   //this shouldn't happen
        if(!rfp->clkSet) {
            return setDV;
        }
        auto dstTm = double (pi.ecr - rfp->zeroTS) * rfp->spTS + rfp->zeroTm;
        if(dstTm > pi.tm) {
        return setDV;
        } else {
            //dv-2 is a noisy estimate of delay from dst through src to CP
            pi.dv[2] = pi.tm - dstTm;
            setDV = true;
        }
        if(clkSet) {
            pi.dv[0] = srcTm - dstTm;
        }
        return setDV;
    }
};
