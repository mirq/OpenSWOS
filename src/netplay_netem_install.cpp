/*
 * netplay_netem_install.cpp -- hand the emulator to GGPO's transport filter.
 *
 * One C++ translation unit for one reason: Udp::SetTransportFilter is a C++
 * static member, and src/netplay_netem.c is C. Keeping the bridge here means
 * the emulator itself stays plain C like the rest of src/, and the library
 * keeps a generic hook rather than a SWOS-shaped one (see
 * third_party/ggpo-x/OPENSWOS.md).
 */
#ifdef SWOS_CHUNKY_BUILD
#ifdef SWOS_NETPLAY

#include "udp.h"
#include "netplay_netem.h"

extern "C" void Netplay_InstallTransportFilter(void)
{
    Udp::SetTransportFilter(&Netem_Send, &Netem_Pump);
}

#endif
#endif
