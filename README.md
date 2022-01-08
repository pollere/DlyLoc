# DlyLoc: delay locator for TCP flows

DlyLoc operates on TCP headers, v4 or v6. It requires the following:

- time of packet capture

- packet IP source, destination, sport, and dport

- TSval and ERC from packet TCP timestamp option

Computes the round trip delay captured packets experience between the packet capture point (CP) to a host  (the "passive ping") when both directions of a flow are available. For single direction flows, dlyloc computes delay variation observed for different segments.

This C++ program uses the libtins library to capture packets or read a packet capture file. This is derived from pping (github.com/pollere/pping) and work on TSDE (transport segment delay estimator) that Pollere originally developed under work supported by the Department of Energy under Award Number DE-SC0009498 . This is a completely passive measurement approach and can be deployed in an end host or any capture point along a path. DlyLoc combines these two techniques: 1) passive ping technique saves the first time a TSval is seen and matches it with the first time that value is seen as a ERC in the reverse direction. Every match produces a round trip time estimate.  2) delay variations are computed for each conforming packet sample. The samples must have TSvals that can be used to extract a reliable estimate of the sending time at the source. There are three *delay variation (dv)* values that can be used to help localize delay. Note that *pping* is a delay metric while *dv* is relative. Packets that produce non-zero metrics are printed on standard output with the format:

- packet capture time

- the *pping* value, if any, for this packet or "-" or -1 if not computable

- the minimum pping *value* (if any) seen so far for this flow

- number of bytes seen from this flow thus far

- dv0: path seg 0 from the destination of this packet to the packet's sender

- dv1: path seg 1 from the sender of this packet to this capture point (CP)

- dv2: path seg2 from the destination of this packet to the sender and back to CP

- flow identifier in the form:  srcIP:port+dstIP:port

It's not always possible to extract a reasonable time estimate from the TSvals and the delay variation from the sender to the CP is the least noisy value. It's possible to use this technique on other observations or to locally (at capture point) save times packets are seen, but this version of DlyLoc only implements the basic approach. Note:

* If unrecoverable source clock, no d0 or d1. If unrecoverable dest clock, no d0 or d2

* This version is not using ECR extracted clocks (usually quite noisy), so must be reverse flow to get dest clock

* Usually, *dv1* can localize which direction of the *pping* is contributing the most delay

DlyLoc implements a "live" or "on-line" approach where some samples are collected before estimation begins and then it is on-going. The number of samples/length of time to collect before producing output is a "best guess": you may wish to experiment.

Output can be in a human-friendly format or a "machine readable" format that is better for using as input to some further analysis. In the former, when a particular metric is not computable a "-" is printed. In the latter, a -1.0 is printed. DlyLoc takes the approach of minimal processing of the data, expecting post-processing to be used to extract friendlier presentations. Several approaches have been used in the past, including awk scripts and graf, d3 javascript, and Qt Qcustomplot plotter programs. For continued live use, output may be redirected to a file or piped to a display or summarization widget. (This author has used tdigest summaries with both qcustomplot and d3.js in the past.)

DlyLoc is meant for network nerds who can get information from the code. There are comments to note where additional data collection could occur but DlyLoc has been kept fairly basic. No support is provided; this code is offered as an example or a basis for the interested.

Some additional information and pictures of processed output are at: http://pollere.net/netobserve.html and http://pollere.net/Pdfdocs/ListeningGoog.pdf.

## Running

Run with -m option for "machine readable" output, e.g.,

`dlyloc -r <>.pcap -m >! out`

For live capture:

`dlyloc -i <interface>`
