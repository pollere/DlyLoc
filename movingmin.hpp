/*
 * movingmin: Track the minimum over a moving interval.
 *
 * For approximate, set subinterval to a nonzero value of suitable granularity
 * by setting the number of spaces per interval
 * Every subinterval, check for a new min using t-axis of int64_t (could use double
 * for more general time notion. This is a general technique but in use by dlyloc,
 * intervals are in TS tick therefore 1ms TS ticks will be ms.
 *
 * Copyright (C) 2021-2  Kathleen Nichols, Pollere LLC
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <https://www.gnu.org/licenses/>.
 *  You may contact Pollere LLC at info@pollere.net.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
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
