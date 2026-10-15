#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
lab::Chans toChans (const juce::AudioBuffer<float>& b, int len)
{
    lab::Chans ch;
    for (int c = 0; c < juce::jmin (2, b.getNumChannels()); ++c) ch.emplace_back (b.getReadPointer (c), b.getReadPointer (c) + len);
    return ch;
}
lab::Chans resampleTo (const lab::Chans& in, double from, double to)
{
    if (std::abs (from - to) < 0.5 || in.empty()) return in;
    lab::Chans out; const double r = from / to;
    for (auto& x : in)
    {
        lab::Buf y ((size_t) std::max (1.0, std::floor ((double) x.size() / r)));
        for (size_t i = 0; i < y.size(); ++i) y[i] = (float) lab::lerpAt (x, (double) i * r);
        out.push_back (std::move (y));
    }
    return out;
}
juce::String safeName (juce::String s) { s = juce::File::createLegalFileName (s.trim()); return s.isEmpty() ? "sample" : s; }
}

DatLabProcessor::DatLabProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true).withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      juce::Thread ("DATLAB render")
{
    formats.registerBasicFormats();
    startTimerHz (10);
}

DatLabProcessor::~DatLabProcessor()
{
    *alive = false;
    stopTimer();
    // The render polls threadShouldExit() on every progress tick, so it ends quickly. Never let JUCE force-kill
    // the thread (TerminateThread on Windows can leave the heap locked and take Ableton down with it).
    signalThreadShouldExit();
    waitForThreadToExit (-1);
}

void DatLabProcessor::markDirty()
{
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
}

void DatLabProcessor::timerCallback()
{
    // a take that stopped by itself (2 minute limit) still becomes the source
    if (capPending && ! capturing.load()) stopCapture();
}

bool DatLabProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    auto out = l.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono()) return false;
    auto in = l.getMainInputChannelSet();
    return in.isDisabled() || in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono();
}

void DatLabProcessor::prepareToPlay (double sampleRate, int) { hostSr = sampleRate; }

void DatLabProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals nd;
    const int n = buffer.getNumSamples(), nIn = getTotalNumInputChannels(), nOut = buffer.getNumChannels();
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto bpm = pos->getBpm()) hostBpm = *bpm;

    // capture the incoming audio
    capWriting = true;   // stopCapture() waits for this block before reading the take
    if (capturing.load() && nIn > 0)
    {
        int p = capPos.load(), m = juce::jmin (n, capBuf.getNumSamples() - p);
        for (int c = 0; c < capBuf.getNumChannels(); ++c) capBuf.copyFrom (c, p, buffer, juce::jmin (c, nIn - 1), 0, m);
        capPos = p + m;
        if (p + m >= capBuf.getNumSamples()) capturing = false;
    }
    capWriting = false;
    // THRU passes the input; a mono input feeds both sides; everything else starts silent
    int passed = thru.load() ? juce::jmin (nIn, nOut) : 0;
    if (passed == 1 && nOut > 1) { buffer.copyFrom (1, 0, buffer, 0, 0, n); passed = 2; }
    for (int c = passed; c < nOut; ++c) buffer.clear (c, 0, n);

    // preview
    const juce::SpinLock::ScopedTryLockType tl (playLock);
    if (! tl.isLocked()) return;   // the message thread is swapping the buffer right now: skip one block
    const int req = playRequest.load();
    if (req != playSeen) { playSeen = req; playPos = 0; }
    auto* pb = playBuf.get();
    if (playing.load() != Play::none && pb != nullptr && ! pb->ch.empty())
    {
        const double step = pb->sr / hostSr.load(); const size_t len = pb->ch[0].size();
        for (int i = 0; i < n; ++i)
        {
            if (playPos >= (double) len - 1)
            {
                if (loopPreview.load()) playPos = 0;
                else { playing = Play::none; break; }
            }
            size_t i0 = (size_t) playPos; float f = (float) (playPos - (double) i0);
            for (int c = 0; c < nOut; ++c)
            {
                const auto& x = pb->ch[(size_t) juce::jmin (c, (int) pb->ch.size() - 1)];
                buffer.addSample (c, i, x[i0] + (x[i0 + 1] - x[i0]) * f);
            }
            playPos += step;
        }
        playFrac = (float) (playPos / (double) juce::jmax<size_t> (1, len));
    }
}

lab::Params& DatLabProcessor::paramsFor (const juce::String& id)
{
    auto it = values.find (id);
    if (it == values.end())
    {
        auto* d = lab::findProc (id.toStdString());
        it = values.emplace (id, d ? lab::defaults (*d) : lab::Params {}).first;
    }
    return it->second;
}
const lab::ProcDef& DatLabProcessor::proc() const { auto* d = lab::findProc (procId.toStdString()); return d ? *d : lab::procs().front(); }

juce::File DatLabProcessor::renderFolder() const
{
    auto f = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("DATLAB");
    f.createDirectory();
    return f;
}

bool DatLabProcessor::loadFile (const juce::File& file, bool asAux, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (file));
    if (! r) { error = "Can't read " + file.getFileName(); return false; }
    const auto len = (int) juce::jmin<juce::int64> (r->lengthInSamples, (juce::int64) (r->sampleRate * 600));
    if (len < 16) { error = file.getFileName() + " is empty"; return false; }
    juce::AudioBuffer<float> b ((int) juce::jmin (2u, r->numChannels), len);
    r->read (&b, 0, len, 0, true, true);
    LabSample s; s.ch = toChans (b, len); s.sr = r->sampleRate; s.name = file.getFileNameWithoutExtension(); s.file = file;
    if (asAux) { aux = std::move (s); status = "Second sample: " + aux.name; }
    else { setSource (std::move (s), false); history.clear(); }
    markDirty();
    sendChangeMessage();
    return true;
}

void DatLabProcessor::setSource (LabSample s, bool pushHistory)
{
    if (pushHistory && ! source.empty()) history.push_back (source);
    if (history.size() > 32) history.erase (history.begin());
    source = std::move (s);
    shape.start = 0; shape.end = 1;
    if (playing.load() == Play::source) stopPlay();
    status = "Source: " + source.name + juce::String::formatted ("  %.2f s  %d Hz  %s", source.seconds(), (int) source.sr, source.ch.size() > 1 ? "stereo" : "mono");
    sendChangeMessage();
}

void DatLabProcessor::chainResult()
{
    if (result.empty()) return;
    LabSample s = result; s.name = result.name;
    setSource (std::move (s), true);
    status = "Result is the new source (UNDO goes back). Stack another process on it.";
    sendChangeMessage();
}

void DatLabProcessor::undoChain()
{
    if (history.empty()) return;
    auto s = std::move (history.back()); history.pop_back();
    setSource (std::move (s), false);
    status = "Back to " + source.name;
    sendChangeMessage();
}

void DatLabProcessor::startProcessing()
{
    if (busy.load()) return;
    if (source.empty()) { status = "No source sample: drop a file, LOAD one or CAPTURE the track"; sendChangeMessage(); return; }
    const auto& d = proc();
    lab::Job job; job.src = source.ch; job.sr = source.sr; job.procId = d.id; job.params = paramsFor (procId); job.shape = shape; job.bpm = effectiveBpm();
    bool needsAux = ! d.aux.empty() && ! (d.id == "mask" && job.params.i ("mode") != 0);
    if (needsAux)
    {
        if (aux.empty()) { status = "This process needs a second sample (" + juce::String (d.aux) + "): drop one on the row under the knobs"; sendChangeMessage(); return; }
        job.aux = resampleTo (aux.ch, aux.sr, source.sr);
    }
    waitForThreadToExit (-1);   // the previous render already finished (busy was cleared after it returned)
    pendingJob = std::move (job); pendingName = source.name; pendingTag = d.id == "robot" && pendingJob.params.i ("mode") == 1 ? "WHSP" : juce::String (d.tag);
    busy = true; progress = 0;
    status = "Processing " + juce::String::fromUTF8 (d.name.c_str()) + "...";
    sendChangeMessage();
    startThread();
}

void DatLabProcessor::cancelProcessing() { if (busy.load()) signalThreadShouldExit(); }

void DatLabProcessor::run()
{
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    auto r = lab::run (pendingJob, [this] (double f) { progress = (float) f; }, [this] { return threadShouldExit(); });
    LabSample res;
    juce::String msg;
    if (! r.error.empty()) msg = r.error == "Cancelled" ? juce::String ("Cancelled") : "Failed: " + juce::String (r.error);
    else
    {
        res.ch = std::move (r.ch); res.sr = r.sr;
        res.name = pendingName.upToFirstOccurrenceOf (" ~", false, false) + " ~" + pendingTag;
        auto folder = renderFolder();
        res.file = folder.getNonexistentChildFile (safeName (res.name), ".wav", true);
        if (! writeWav (res, res.file).existsAsFile()) msg = "Rendered, but couldn't write " + res.file.getFullPathName();
        else msg = juce::String::formatted ("Done in %.1f s  -  %.2f s result  -  ", (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0, res.seconds())
                   + juce::String (juce::CharPointer_UTF8 ("drag it to Ableton, PLAY RESULT to listen, CHAIN to stack another process"));
    }
    juce::MessageManager::callAsync ([this, aliveFlag = alive, rendered = std::move (res), msg]() mutable
    {
        if (! aliveFlag->load()) return;
        if (! rendered.empty()) { result = std::move (rendered); if (playing.load() == Play::result) stopPlay(); markDirty(); }
        status = msg; busy = false; sendChangeMessage();
    });
}

juce::File DatLabProcessor::writeWav (const LabSample& s, const juce::File& f)
{
    f.deleteFile();
    std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
    if (! os) return {};
    juce::WavAudioFormat wav;
    auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (s.sr).withNumChannels ((int) s.ch.size()).withBitsPerSample (24));
    if (! w) return {};
    std::vector<const float*> ptrs; for (auto& c : s.ch) ptrs.push_back (c.data());
    w->writeFromFloatArrays (ptrs.data(), (int) ptrs.size(), (int) s.ch[0].size());
    w.reset();
    return f;
}

void DatLabProcessor::randomize()
{
    auto& p = paramsFor (procId); juce::Random rnd;
    for (auto& pd : proc().params)
    {
        double u = rnd.nextDouble(), v;
        if (pd.flags & lab::PExp && pd.min > 0) v = pd.min * std::pow (pd.max / pd.min, u);
        else v = pd.min + (pd.max - pd.min) * u;
        if (pd.isInt()) v = std::round (v);
        p.v[pd.id] = v;
    }
    sendChangeMessage();
}
void DatLabProcessor::resetParams() { values[procId] = lab::defaults (proc()); sendChangeMessage(); }

void DatLabProcessor::play (Play what)
{
    const LabSample& s = what == Play::result ? result : source;
    if (s.empty()) return;
    auto pb = std::make_unique<PlayBuf>();
    if (what == Play::source)
    {   // only the selected region
        size_t len = s.ch[0].size(), a = juce::jmin (len - 1, (size_t) (shape.start * (double) len)), b = juce::jlimit (a + 2, len, (size_t) (shape.end * (double) len));
        if (b <= a + 1) return;
        for (auto& c : s.ch) pb->ch.emplace_back (c.begin() + (long) a, c.begin() + (long) b);
    }
    else pb->ch = s.ch;
    for (auto& c : pb->ch) c.push_back (0.0f);
    pb->sr = s.sr;
    {
        const juce::SpinLock::ScopedLockType l (playLock);
        std::swap (playBuf, pb);   // the old buffer is freed below, on this (message) thread
        playFrac = 0;
        ++playRequest;
        playing = what;
    }
}
void DatLabProcessor::stopPlay() { playing = Play::none; }
float DatLabProcessor::playPosition() const { return playFrac.load(); }

void DatLabProcessor::startCapture()
{
    capSr = hostSr.load();
    if (capBuf.getNumSamples() != (int) (capSr * 120)) capBuf.setSize (2, (int) (capSr * 120));
    capBuf.clear();
    capPos = 0; capturing = true; capPending = true;
    status = "Capturing the track input... press CAPTURE again to stop (max 2 min)";
    sendChangeMessage();
}

void DatLabProcessor::stopCapture()
{
    capturing = false;
    capPending = false;
    while (capWriting.load()) juce::Thread::yield();   // the audio thread may still be finishing a block
    const int len = capPos.load();
    if (len < 64) { status = "Nothing captured: is audio playing into this track?"; sendChangeMessage(); return; }
    LabSample s; s.ch = toChans (capBuf, len); s.sr = capSr; s.name = "CAPTURE " + juce::Time::getCurrentTime().formatted ("%H%M%S");
    auto folder = renderFolder().getChildFile ("Captures"); folder.createDirectory();
    s.file = writeWav (s, folder.getNonexistentChildFile (s.name, ".wav", true));
    setSource (std::move (s), true);
    markDirty();
}

// ---- state (saved in the Ableton set) ----
void DatLabProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::XmlElement x ("DATLAB");
    x.setAttribute ("version", 1);
    x.setAttribute ("proc", procId);
    x.setAttribute ("bpmHost", bpmFromHost); x.setAttribute ("bpm", manualBpm);
    x.setAttribute ("thru", thru.load()); x.setAttribute ("loop", loopPreview.load());
    auto* sh = x.createNewChildElement ("Shape");
    sh->setAttribute ("start", shape.start); sh->setAttribute ("end", shape.end); sh->setAttribute ("inGain", shape.inGain); sh->setAttribute ("revIn", shape.revIn);
    sh->setAttribute ("mix", shape.mix); sh->setAttribute ("revOut", shape.revOut); sh->setAttribute ("lowCut", shape.lowCut); sh->setAttribute ("highCut", shape.highCut);
    sh->setAttribute ("width", shape.width); sh->setAttribute ("fadeIn", shape.fadeIn); sh->setAttribute ("fadeOut", shape.fadeOut); sh->setAttribute ("loopXfade", shape.loopXfade);
    sh->setAttribute ("trim", shape.trim); sh->setAttribute ("normalize", shape.normalize); sh->setAttribute ("outGain", shape.outGain); sh->setAttribute ("maxLen", shape.maxLen);
    for (auto& [id, p] : values)
    {
        auto* e = x.createNewChildElement ("Params"); e->setAttribute ("proc", id);
        for (auto& [k, v] : p.v) e->setAttribute (juce::String (k), v);
    }
    auto file = [&] (const char* tag, const LabSample& s) { if (s.file.existsAsFile()) { auto* e = x.createNewChildElement (tag); e->setAttribute ("path", s.file.getFullPathName()); e->setAttribute ("name", s.name); } };
    file ("Source", source); file ("Aux", aux); file ("Result", result);
    copyXmlToBinary (x, dest);
}

void DatLabProcessor::setStateInformation (const void* data, int size)
{
    auto x = getXmlFromBinary (data, size);
    if (! x || ! x->hasTagName ("DATLAB")) return;
    procId = x->getStringAttribute ("proc", "paul");
    if (! lab::findProc (procId.toStdString())) procId = "paul";
    bpmFromHost = x->getBoolAttribute ("bpmHost", true); manualBpm = x->getDoubleAttribute ("bpm", 120);
    thru = x->getBoolAttribute ("thru", true); loopPreview = x->getBoolAttribute ("loop", false);
    if (auto* sh = x->getChildByName ("Shape"))
    {
        shape.start = sh->getDoubleAttribute ("start", 0); shape.end = sh->getDoubleAttribute ("end", 1); shape.inGain = sh->getDoubleAttribute ("inGain");
        shape.revIn = sh->getBoolAttribute ("revIn"); shape.mix = sh->getDoubleAttribute ("mix", 1); shape.revOut = sh->getBoolAttribute ("revOut");
        shape.lowCut = sh->getDoubleAttribute ("lowCut", 20); shape.highCut = sh->getDoubleAttribute ("highCut", 20000); shape.width = sh->getDoubleAttribute ("width", 1);
        shape.fadeIn = sh->getDoubleAttribute ("fadeIn", 0); shape.fadeOut = sh->getDoubleAttribute ("fadeOut", 5); shape.loopXfade = sh->getDoubleAttribute ("loopXfade", 0);
        shape.trim = sh->getBoolAttribute ("trim"); shape.normalize = sh->getBoolAttribute ("normalize", true); shape.outGain = sh->getDoubleAttribute ("outGain");
        shape.maxLen = sh->getDoubleAttribute ("maxLen", 180);
    }
    values.clear();
    for (auto* e : x->getChildWithTagNameIterator ("Params"))
    {
        auto id = e->getStringAttribute ("proc"); auto* d = lab::findProc (id.toStdString()); if (! d) continue;
        lab::Params p = lab::defaults (*d);
        for (auto& pd : d->params) if (e->hasAttribute (juce::String (pd.id))) p.v[pd.id] = e->getDoubleAttribute (juce::String (pd.id));
        values[id] = p;
    }
    const auto keepShape = shape;
    juce::String err;
    auto load = [&] (const char* tag, int which)
    {
        if (auto* e = x->getChildByName (tag))
        {
            juce::File f (e->getStringAttribute ("path"));
            if (! f.existsAsFile()) return;
            if (which == 2)
            {
                std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (f)); if (! r) return;
                juce::AudioBuffer<float> b ((int) juce::jmin (2u, r->numChannels), (int) r->lengthInSamples); r->read (&b, 0, b.getNumSamples(), 0, true, true);
                result.ch = toChans (b, b.getNumSamples()); result.sr = r->sampleRate; result.file = f; result.name = e->getStringAttribute ("name", f.getFileNameWithoutExtension());
            }
            else if (loadFile (f, which == 1, err) && which == 0) source.name = e->getStringAttribute ("name", source.name);
        }
    };
    load ("Source", 0); load ("Aux", 1); load ("Result", 2);
    shape = keepShape;
    sendChangeMessage();
}

juce::AudioProcessorEditor* DatLabProcessor::createEditor() { return new DatLabEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new DatLabProcessor(); }
