// The Composers Desktop Project (CDP) collection: processes modelled on CDP8's programs
// (https://github.com/ComposersDesktop/CDP8, LGPL-2.1). Each batch lives in its own LabCdp*.cpp
// and appends its processes to the registry.
#pragma once
#include "LabDefsUtil.h"

namespace lab
{
void addCdp (std::vector<ProcDef>& v);   // LabCdp.cpp: every batch, in menu order
void addCdpSpectralA (std::vector<ProcDef>& v);   // LabCdpSpecA.cpp
void addCdpSpectralB (std::vector<ProcDef>& v);   // LabCdpSpecB.cpp
void addCdpCross (std::vector<ProcDef>& v);   // LabCdpCross.cpp
void addCdpWaveset (std::vector<ProcDef>& v);   // LabCdpWave.cpp
void addCdpExtend (std::vector<ProcDef>& v);   // LabCdpExtend.cpp
void addCdpGrain (std::vector<ProcDef>& v);   // LabCdpGrain.cpp
void addCdpEnvFilter (std::vector<ProcDef>& v);   // LabCdpEnvFilt.cpp
void addCdpSpace (std::vector<ProcDef>& v);   // LabCdpSpace.cpp
}
