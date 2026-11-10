// SPDX-License-Identifier: GPL-2.0-only
/** Same CLOCK_MONOTONIC domain as the Java transaction; /proc/uptime includes suspend. */
public final class LatencyMonotonicMain {
    public static void main(String[] args){if(args.length!=0)System.exit(64);long now=System.nanoTime()/1000000L;if(now<0)System.exit(1);System.out.println(now);}
}
