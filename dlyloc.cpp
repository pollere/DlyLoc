/*
 * dlyloc - Pollere Basic Delay Estimator and Locator for TCP flows
 *
 * Copyright (C) 2022 Pollere LLC
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

/*
 Usage:
    dlyloc -i interfacename -m
 or
    dlyloc -r pcapfilename
 
 Typing dlyloc without arguments gives a list of available optional arguments.

 dlyloc is provided as sample code for a basic segment delay locator.
 It is NOT intended as production code.

 dlyloc operates on TCP headers, v4 or v6. It requires the
 following:
 - time of packet capture
 - packet IP source, destination, sport, and dport
 - TSval and ERC from packet TCP timestamp option

 When both directions of a flow are available, computes the round trip delay captured
 packets experience between the packet capture point (CP) to a host (the "passive ping").
 Otherwise will output delay variation observed for different segments.

 dlyloc combines two techniques: 1) passive ping technique saves the first time a TSval
 is seen and matches it with the first time that value is seen as a ERC in the
 reverse direction. Every match produces a round trip time estimate. 2) delay
 variations are computed for each conforming packet sample. The samples must have
 TSvals that can be used to extract a reliable estimate of the sending time at the
 source. This implements a "live" or "on-line" approach so samples are collected before
 estimation begins. The number of samples/length of time is just a "best guess": you
 may wish to experiment.

 Packets that produce non-zero metrics are printed on standard output with the format:
    - packet capture time
    - round trip delay observed for this packet or "-" or -1 if not computed
    - shortest round trip delay seen so far for this flow
    - number of bytes seen from this flow so far
    - delay variation from the destination of this packet to the packet's sender
    - delay variation from the sender of this packet to this capture point
    - delay variation from the destination of this packet to the sender and back to CP
    - flow identifier in the form:  srcIP:port+dstIP:port

 It's not always possible to extract a reasonable time estimate from the TSvals and the
 delay variation from the sender to the CP is the least noisy value. It's possible to use
 this technique on other observations or to locally (at capture point) save times packets
 are seen, but these files only have the basic approach.

 For continued live use, output may be redirected to a file or piped to a display or
 summarization widget. (This author has used tdigest summaries with both qcustomplot and
 d3.js in the past.)
 */

#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <pcap.h>
#include <ctime>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <cmath>
#include "tins/tins.h"

#include "./movingmin.hpp"
#include "./flowDelay.hpp"

using namespace Tins;

#define SNAP_LEN 144                // maximum bytes per packet to capture
static double tsvalMaxAge = 10.;    // limit age of TSvals to use
static double flowMaxIdle = 300.;   // flow idle time until flow forgotten
static double sumInt = 10.;         // how often (sec) to print summary line
static int maxFlows = 10000;
static int flowCnt;
static std::unordered_map<std::string, flowDly*> flows;
static double time_to_run;      // how many seconds to capture (0=no limit)
static int maxPackets;          // max packets to capture (0=no limit)
static int64_t offTm = -1;      // first packet capture time (used to
                                // avoid precision loss when 52 bit timestamp
                                // normalized into FP double 47 bit mantissa)
static bool machineReadable = false; // machine or human readable output
static double capTm, startm;        // (in seconds)
static int pktCnt, not_tcp, no_TS, not_v4or6, uniDir;
static std::string localIP;         // ignore pp through this address
static bool filtLocal = true;
static std::string filter("tcp");    // default bpf filter
static int64_t flushInt = 1 << 20;  // stdout flush interval (~uS)
static int64_t nextFlush;       // next stdout flush time (~uS)

// save capture time of packet using its flow + TSval as key.  If key
// exists, don't change it.  The same TSval may appear on multiple
// packets so this retains the first (oldest) appearance which may
// overestimate RTT but won't underestimate. This slight bias may be
// reduced by adding additional fields to the key (such as packet's
// ending tcp_seq to match against returned tcp_ack) but this can
// substantially increase the state burden for a small improvement.

static std::unordered_map<std::string, double> tsTbl;

static inline void addTS(const std::string& key, double t)
{
#ifdef __cpp_lib_unordered_map_try_emplace
    tsTbl.try_emplace(key, t);
#else
    if (tsTbl.count(key) == 0) {
        tsTbl.emplace(key, t);
    }
#endif
}

// A packet's ECR (timestamp echo reply) should match the TSval of some
// packet seen earlier in the flow's reverse direction so lookup the
// capture time recorded above using the reversed flow + ECR as key. If
// found, the difference between now and capture time of that packet is
// >= the current RTT. Multiple packets may have the same ECR but the
// first packet's capture time gives the best RTT estimate so the time
// in the entry is negated after retrieval to prevent reuse.  The entry
// can't be deleted yet because TSvals may change on time scales longer
// than the RTT so a deleted entry could be recreated by a later packet
// with the same TSval which could match an ECR from an earlier
// incarnation resulting in a large RTT underestimate.  Table entries
// are deleted after a time interval (tsvalMaxAge) that should be:
//  a) longer than the largest time between TSval ticks
//  b) longer than longest queue wait packets are expected to experience

static inline double getTStm(const std::string& key)
{
    try {
        auto d = tsTbl.at(key);
        tsTbl[key] = -d;     //mark it negative to indicate it's been used
        return d;
    } catch (std::out_of_range) {
        return -1.;
    }
}

static std::string fmtTimeDiff(double dt)
{
    const char* SIprefix = "";
    if (dt < 1e-3) {
        dt *= 1e6;
        SIprefix = "u";
    } else if (dt < 1) {
        dt *= 1e3;
        SIprefix = "m";
    } 
    const char* fmt;
    if (dt < 10.) {
        fmt = "%.2lf%ss";
    } else if (dt < 100.) {
        fmt = "%.1lf%ss";
    } else {
        fmt = " %.0lf%ss";
    }
    char buf[10];
    snprintf(buf, sizeof(buf), fmt, dt, SIprefix);
    return buf;
}

/*
 * return (approximate) time in a 64bit fixed point integer with the
 * binary point at bit 20. High accuracy isn't needed (this time is
 * only used to control output flushing) so time is stretched ~5%
 * ((1024^2)/1e6) to avoid a 64 bit multiply.
 */
static int64_t clock_now(void) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t(tv.tv_sec) << 20) | tv.tv_usec;
}

static void processPacket(const Packet& pkt)
{
    pktCnt++;
    // all packets should be TCP since that's in config
    const TCP* t_tcp;
    if ((t_tcp = pkt.pdu()->find_pdu<TCP>()) == nullptr) {
        not_tcp++;
        return;
    }
    std::string tsVal, ecrVal;
    uint32_t ts, ecr;
    try {
        std::pair<uint32_t, uint32_t> tts = t_tcp->timestamp();
        tsVal = std::to_string(tts.first);
        ecrVal = std::to_string(tts.second);
        ts = tts.first;
        ecr = tts.second;
    } catch (std::exception&) {
        no_TS++;
        return;
    }
    if (ts == 0 || (ecr == 0 && (t_tcp->flags() != TCP::SYN))) {
        return;
    }

    const IP* ip;
    pktInfo pi;
    std::string dstHst;
    const IPv6* ipv6;
    if ((ip = pkt.pdu()->find_pdu<IP>()) != nullptr) {
        pi.IPsrc = ip->src_addr().to_string() + ":" + std::to_string(t_tcp->sport());
        dstHst = ip->dst_addr().to_string();
        pi.IPdst = dstHst + ":" + std::to_string(t_tcp->dport());
    } else if ((ipv6 = pkt.pdu()->find_pdu<IPv6>()) != nullptr) {
        pi.IPsrc = ipv6->src_addr().to_string() + ":" + std::to_string(t_tcp->sport());
        dstHst = ipv6->dst_addr().to_string();
        pi.IPdst = dstHst + ":" + std::to_string(t_tcp->dport());
    } else {
        not_v4or6++;
        return;
    }
    /*
     * Reach here with a TCP packet with timestamp option
     * Process capture clock time
     */
    std::time_t result = pkt.timestamp().seconds();
    if (offTm < 0) {
        offTm = static_cast<int64_t>(pkt.timestamp().seconds());
        // fractional part of first usable packet time
        startm = double(pkt.timestamp().microseconds()) * 1e-6;
        capTm = startm;
        if (sumInt) {
            std::cerr << "First packet at " << std::asctime(std::localtime(&result)) << "\n";
        }
    } else {
        // offset capture time
        int64_t tt = static_cast<int64_t>(pkt.timestamp().seconds()) - offTm;
        capTm = double(tt) + double(pkt.timestamp().microseconds()) * 1e-6;
    }

    std::string fstr = pi.IPsrc + "+" + pi.IPdst;  // could add DSCP field to key

    // Creates a flowDly entry whenever needed
    flowDly* fr;
    if (flows.count(fstr) == 0u) {
        if (flowCnt > maxFlows) {
            return;                 // stop adding flows till something goes away
        }
        fr = new flowDly(fstr);
        fr->startTm =  capTm;
        fr->startTS = pi.ts = extendTS(ts, &(fr->twrap));
        fr->startTS = pi.ts;
        flowCnt++;
        flows.emplace(fstr, fr);
        // only record tsvals when capturing both directions of a flow
        // if this flow is the reverse of a known flow, mark both as bi-directional
        if (flows.count(pi.IPdst + "+" + pi.IPsrc) != 0u) {
            flowDly* rfr = flows.at(pi.IPdst + "+" + pi.IPsrc);
            if (rfr == nullptr)
                std::cerr << "Shouldn't be a nullptr\n";
            rfr->revFlow = true;
            rfr->rfp = fr;
            fr->revFlow = true;
            fr->rfp = rfr;
        }
    } else {
        try {
            fr = flows.at(fstr);
            pi.ts = extendTS(ts, &(fr->twrap));
        } catch (std::out_of_range) {   //shouldn't happen since tested first
            std::cerr << "processPacket: no flow in map though count is non-zero\n";
            return;
        }
    }
    pi.tm = fr->_lastTm = capTm;
    pi.sz = pkt.pdu()->size();    
    pi.ecr = extendTS(ecr, &(fr->ewrap));
    pi.dv[0] = pi.dv[1] = pi.dv[2] = -1.;
    fr->bytesSnt += (double)pi.sz;
    fr->pktCnt++;
    bool dvs = fr->computeDV(pi);
    double outTm = -1.;   //time of outbound pping match packet
    if(fr->revFlow) {
        outTm = getTStm(pi.IPdst + "+" + pi.IPsrc + "+" + ecrVal);
        if (!filtLocal || (localIP != dstHst)) {    //track for ppings
            addTS(fstr + "+" + tsVal, capTm);
        }
    } else
        uniDir++;

    //output metrics [would like to consolidate print stuff into a printPacket method]
    if (dvs && (!fr->revFlow || outTm < 0.)) { //check for no pping for this sample
        if (machineReadable) {
            printf("%" PRId64 ".%06d -1 -1 %.0f %.6f %.6f %.6f",
                int64_t(capTm + offTm), int((capTm - floor(capTm)) * 1e6), fr->bytesSnt,
                        pi.dv[0], pi.dv[1], pi.dv[2]);
        } else {
                char tbuff[80];
                struct tm* ptm = std::localtime(&result);
                strftime(tbuff, 80, "%T", ptm);
                printf("%s - -", tbuff);
                for(int i=0; i < 3; i++) {
                    if(pi.dv[i] > -1.)
                        printf(" %s", fmtTimeDiff((double)pi.dv[i]).c_str());
                    else
                        printf(" -");
                }

       }
    } else if(outTm > 0.) {    //this is a return pping
        // this packet is a return "pping" -- process it for packet's src
        double rtt = capTm - outTm;
        if (fr->_minPP > rtt)  {
            fr->_minPP = rtt; // reset minimum
            fr->_minTS = pi.ts - fr->startTS;
            fr->_minTm = capTm;
        }
        if (machineReadable) {
            printf("%" PRId64 ".%06d %.6f %.6f %.0f %.6f %.6f %.6f",
                int64_t(capTm + offTm), int((capTm - floor(capTm)) * 1e6),
                    rtt, fr->_minPP, fr->bytesSnt, pi.dv[0], pi.dv[1], pi.dv[2]);
        } else {
            char tbuff[80];
            struct tm* ptm = std::localtime(&result);
            strftime(tbuff, 80, "%T", ptm);
            printf("%s %s %s", tbuff, fmtTimeDiff(rtt).c_str(),
               fmtTimeDiff(fr->_minPP).c_str());
            for(int i=0; i < 3; i++) {
                if(pi.dv[i] > -1.)
                    printf(" %s", fmtTimeDiff((double)pi.dv[i]).c_str());
                else
                    printf(" -");
            }
        }
    } else
        return; //no metrics to print

    printf(" %s\n", fstr.c_str());
    int64_t now = clock_now();
    if (now - nextFlush >= 0) {
        nextFlush = now + flushInt;
        fflush(stdout);
    }
}

static void cleanUp(double n)
{
    // erase entry if its TSval was seen more than tsvalMaxAge
    // seconds in the past. 
    for (auto it = tsTbl.begin(); it != tsTbl.end();) {
        if (capTm - std::abs(it->second) > tsvalMaxAge) {
            it = tsTbl.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = flows.begin(); it != flows.end();) {
        flowDly* fr = it->second;
        if (n - fr->_lastTm > flowMaxIdle) {
            if(fr->revFlow) {
                fr->rfp->revFlow = false;
                fr->rfp->rfp = nullptr;
            }
            delete it->second;
            it = flows.erase(it);
            flowCnt--;
            continue;
        }
        ++it;
    }
}

// return the local ip address of 'ifname'
// XXX since an interface can have multiple addresses, both IP4 and IP6,
// this should really create a set of all of them and later test for
// membership. But for now we just take the first IP4 address.
static std::string localAddrOf(const std::string ifname)
{
    std::string local{};
    struct ifaddrs* ifap;

    if (getifaddrs(&ifap) == 0) {
        for (auto ifp = ifap; ifp; ifp = ifp->ifa_next) {
            if (ifname == ifp->ifa_name &&
                  ifp->ifa_addr->sa_family == AF_INET) {
                uint32_t ip = ((struct sockaddr_in*)
                               ifp->ifa_addr)->sin_addr.s_addr;
                local = IPv4Address(ip).to_string();
                break;
            }
        }
        freeifaddrs(ifap);
    }
    return local;
}

static inline std::string printnz(int v, const char *s) {
    return (v > 0? std::to_string(v) + s : "");
}

static void printSummary()
{
    std::cerr << flowCnt << " flows, "
              << pktCnt << " packets, " +
                 printnz(no_TS, " no TS opt, ") +
                 printnz(uniDir, " uni-directional, ") +
                 printnz(not_tcp, " not TCP, ") +
                 printnz(not_v4or6, " not v4 or v6, ") +
                 "\n";
}

static struct option opts[] = {
    { "interface", required_argument, nullptr, 'i' },
    { "read",      required_argument, nullptr, 'r' },
    { "filter",    required_argument, nullptr, 'f' },
    { "count",     required_argument, nullptr, 'c' },
    { "seconds",   required_argument, nullptr, 's' },
    { "quiet",     no_argument,       nullptr, 'q' },
    { "verbose",   no_argument,       nullptr, 'v' },
    { "showLocal", no_argument,       nullptr, 'l' },
    { "machine",   no_argument,       nullptr, 'm' },
    { "sumInt",    required_argument, nullptr, 'S' },
    { "tsvalMaxAge", required_argument, nullptr, 'M' },
    { "flowMaxIdle", required_argument, nullptr, 'F' },
    { "help",      no_argument,       nullptr, 'h' },
    { 0, 0, 0, 0 }
};

static void usage(const char* pname) {
    std::cerr << "usage: " << pname << " [flags] -i interface | -r pcapFile\n";
}

static void help(const char* pname) {
    usage(pname);
    std::cerr << " flags:\n"
"  -i|--interface ifname   do live capture from interface <ifname>\n"
"\n"
"  -r|--read pcap     process capture file <pcap>\n"
"\n"
"  -f|--filter expr   pcap filter applied to packets.\n"
"                     Eg., \"-f 'net 74.125.0.0/16 or 45.57.0.0/17'\"\n" 
"                     only shows traffic to/from youtube or netflix.\n"
"\n"
"  -m|--machine       'machine readable' output format suitable\n"
"                     for graphing or post-processing. Timestamps\n"
"                     are printed as seconds since capture start.\n"
"                     RTT and minRTT are printed as seconds. All\n"
"                     times have a resolution of 1us (6 digits after\n"
"                     decimal point).\n"
"\n"
"  -c|--count num     stop after capturing <num> packets\n"
"\n"
"  -s|--seconds num   stop after capturing for <num> seconds \n"
"\n"
"  -q|--quiet         don't print summary reports to stderr\n"
"\n"
"  -v|--verbose       print summary reports to stderr every sumInt (10) seconds\n"
"\n"
"  -l|--showLocal     show RTTs through local host applications\n"
"\n"
"  --sumInt num       summary report print interval (default 10s)\n"
"\n"
"  --tsvalMaxAge num  max age of an unmatched tsval (default 10s)\n"
"\n"
"  --flowMaxIdle num  flows idle longer than <num> are deleted (default 300s)\n"
"\n"
"  -h|--help          print help then exit\n"
;
}

int main(int argc, char* const* argv)
{
    bool liveInp = false;
    std::string fname;
    if (argc <= 1) {
        help(argv[0]);
        exit(1);
    }
    for (int c; (c = getopt_long(argc, argv, "i:r:f:c:s:hlmqv",
                                 opts, nullptr)) != -1; ) {
        switch (c) {
        case 'i': liveInp = true; fname = optarg; break;
        case 'r': fname = optarg; break;
        case 'f': filter += " and (" + std::string(optarg) + ")"; break;
        case 'c': maxPackets = atof(optarg); break;
        case 's': time_to_run = atof(optarg); break;
        case 'q': sumInt = 0.; break;
        case 'v': break; // summary on by default
        case 'l': filtLocal = false; break;
        case 'm': machineReadable = true; break;
        case 'S': sumInt = atof(optarg); break;
        case 'M': tsvalMaxAge = atof(optarg); break;
        case 'F': flowMaxIdle = atof(optarg); break;
        case 'h': help(argv[0]); exit(0);
        }
    }
    if (optind < argc || fname.empty()) {
        usage(argv[0]);
        exit(1);
    }

    BaseSniffer* snif;
    {
        SnifferConfiguration config;
        config.set_filter(filter);
        config.set_promisc_mode(false);
        config.set_snap_len(SNAP_LEN);
        config.set_timeout(250);

        try {
            if (liveInp) {
                snif = new Sniffer(fname, config);
                if (filtLocal) {
                    localIP = localAddrOf(fname);
                    if (localIP.empty()) {    
                        filtLocal = false;  // couldn't get local ip addr
                    }
                }
            } else {
                snif = new FileSniffer(fname, config);
            }
        } catch (std::exception& ex) {
            std::cerr << "Couldn't open " << fname << ": " << ex.what() << "\n";
            exit(EXIT_FAILURE);
        }
    }
    if (liveInp && machineReadable) {
        // output every 100ms when piping to analysis/display program
        flushInt /= 10;
    }
    nextFlush = clock_now() + flushInt;
    double nxtSum = 0., nxtClean = 0.;

    for (const auto& packet : *snif) {
        processPacket(packet);

        if ((time_to_run > 0. && capTm - startm >= time_to_run) ||
            (maxPackets > 0 && pktCnt >= maxPackets)) {
            printSummary();
            std::cerr << "Captured " << pktCnt << " packets in "
                      << (capTm - startm) << " seconds\n";
            break;
        }
        if (capTm >= nxtSum && sumInt) {
            if (nxtSum > 0.) {
                printSummary();
                pktCnt = 0;
                no_TS = 0;
                uniDir = 0;
                not_tcp = 0;
                not_v4or6 = 0;
            }
            nxtSum = capTm + sumInt;

        }
        if (capTm >= nxtClean) {
            cleanUp(capTm);  // get rid of stale entries
            nxtClean = capTm + tsvalMaxAge;
        }
    }
    exit(0);
}
