// CDP collection, Batch 4 - Waveset / sample-level distortion + pitch-synchronous grains.
#include "LabCdp.h"
#include "LabDspUtil.h"

namespace lab
{
namespace
{
using namespace lab::dsp;
} // namespace

void addCdpWaveset (std::vector<ProcDef>& v)
{
    using namespace defs;
    const std::string CAT = "CDP \xc2\xb7 WAVESET \xc2\xb7 PSOLA";
    (void) v; (void) CAT;
}
} // namespace lab
