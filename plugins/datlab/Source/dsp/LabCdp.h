// The Composers Desktop Project (CDP) collection: processes modelled on CDP8's programs
// (https://github.com/ComposersDesktop/CDP8, LGPL-2.1). Each batch lives in its own LabCdp*.cpp
// and appends its processes to the registry.
#pragma once
#include "LabDefsUtil.h"

namespace lab
{
void addCdp (std::vector<ProcDef>& v);   // LabCdp.cpp: every batch, in menu order
}
