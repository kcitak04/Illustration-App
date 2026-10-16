#include "LabCdp.h"

namespace lab
{
void addCdp (std::vector<ProcDef>& v)
{
    addCdpSpectralA (v);
    addCdpSpectralB (v);
    addCdpCross (v);
    addCdpWaveset (v);
    addCdpExtend (v);
    addCdpGrain (v);
    addCdpEnvFilter (v);
    addCdpSpace (v);
}
}
