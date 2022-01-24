/*
 * movingmin: Track the minimum over a moving interval.
 *
 * For approximate, set subinterval to a nonzero value of suitable granularity
 * by setting the number of spaces per interval
 * Every subinterval, check for a new min using t-axis of int64_t (could use double
 * for more general time notion. This is a general technique but in use by dlyloc,
 * intervals are in TS tick therefore 1ms TS ticks will be ms.
 */

/*
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

#include <inttypes.h>
#include <string>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>

using minSamp = std::pair<double, int64_t>;
const int64_t interval = 100;    //~100 ms
const double intervalSpaces = 5.;

struct movingMin {
    std::vector<minSamp>  _minList{};
    double _nxtIntr{};
    int64_t _interval;
    int64_t _sub;

    movingMin(double is=intervalSpaces, int64_t i= interval) : _interval{i}
    { _sub = i/is; }

    void addSample(auto v, auto t)
    {
    if(_minList.empty() || v <= _minList.front().first || t > _minList.back().second + _interval) {
        _minList.clear();
        _minList.emplace_back(v,t);
        return;
    }
    auto frst = 0;  //first in current interval
    for(auto i=0; i< _minList.size(); i++)
    {
        if((_minList[i].second + _interval) >= t) {
            frst = i;
            break;
        }
    }
    if(frst != 0) { //checked last item already so frst has to be at most size+1
        _minList.erase(_minList.begin(), _minList.begin()+frst);
    }
    if(v > _minList.back().first) {
        if (_minList.back().second + _sub < t) {
            _minList.emplace_back(v,t);
        }
        return;
    }
    for(auto i=0; i< _minList.size(); i++)
    {
        if(v <= _minList[i].first) {
            _minList.resize(i);
            _minList.emplace_back(v,t);
            return;
        }
    }
    return; //shouldn't get here since v should be <= to at least end value
    }

    bool newInterval(uint64_t t) {
        if(t < _nxtIntr) return false;
        while(_nxtIntr <= t) _nxtIntr += _interval;
        return true;
    }

    void setFirstInterval(int64_t t = 0) { _nxtIntr = t+_interval;}

    const minSamp intervalMin() { return {_minList.front().first, _minList.front().second};}
};

/*
int main()
{
    std::ifstream in("tstMin");
    double startTm = -1.;
    movingMin mm{};
    for (std::string ln; getline(in, ln); )
    {
        auto pos = ln.find(" ");
        auto t = std::stof(ln.substr(0, pos));
        auto v = std::stof(ln.substr(pos+1));
        if(startTm < 0.) {
            startTm = t;
            mm.setFirstInterval(startTm);
        }
        mm.addSample(v, t);
        if(mm.newInterval(t)) {
            minSamp p = mm.intervalMin();
            std::cout << p.second << " " << p.first <<"\n";
        }
   }
}
*/
