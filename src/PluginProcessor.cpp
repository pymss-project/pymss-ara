#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
juce::AudioProcessor::BusesProperties PyMSSProcessorImpl::getBusesProperties()
{
    // 1 stereo input + 1 stereo output (monitor mix of stems).
    juce::AudioProcessor::BusesProperties props;
    props.addBus (true, "Input", juce::AudioChannelSet::stereo(), true);
    props.addBus (false, "Output", juce::AudioChannelSet::stereo(), true);
    return props;
}

PyMSSProcessorImpl::PyMSSProcessorImpl()
    : juce::AudioProcessor (getBusesProperties())
{
}

PyMSSDocumentController* PyMSSProcessorImpl::getDC()
{
    // PlugInExtension::getDocumentController() is inherited by AudioProcessorARAExtension
    // (but not re-declared, unlike getPlaybackRenderer), so qualify through the base to
    // resolve the template. Returns ARA::PlugIn::DocumentController* regardless of role.
    auto* dc = this->ARA::PlugIn::PlugInExtension::getDocumentController();
    if (dc == nullptr)
        return nullptr;

    return juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<PyMSSDocumentController> (dc);
}

juce::ARAAudioSource* PyMSSProcessorImpl::getPrimaryAudioSource()
{
    if (auto* r = getPlaybackRenderer<PyMSSPlaybackRenderer>())
    {
        for (auto* playbackRegion : r->getPlaybackRegions())
        {
            if (playbackRegion != nullptr && playbackRegion->getAudioModification() != nullptr)
                if (auto* src = playbackRegion->getAudioModification()->getAudioSource())
                    return src;
        }
    }
    return nullptr;
}

//==============================================================================
void PyMSSProcessorImpl::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    prepareToPlayForARA (sampleRate, samplesPerBlock, getTotalNumOutputChannels(), getProcessingPrecision());
}

void PyMSSProcessorImpl::releaseResources()
{
    releaseResourcesForARA();
}

bool PyMSSProcessorImpl::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Single mono/stereo output bus; optional matching input bus.
    const auto& out = layouts.getMainOutputChannelSet();
    if (out.isDisabled())
        return false;
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    const auto& in = layouts.getMainInputChannelSet();
    if (! in.isDisabled() && in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo())
        return false;

    return true;
}

void PyMSSProcessorImpl::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    midi.clear();

    auto* playHead = getPlayHead();
    if (! processBlockForARA (buffer, isRealtime(), playHead))
    {
        // Not running in ARA mode: nothing to produce.
        buffer.clear();
    }
}

double PyMSSProcessorImpl::getTailLengthSeconds() const
{
    double tail = 0.0;
    if (getTailLengthSecondsForARA (tail))
        return tail;
    return 0.0;
}

//==============================================================================
void PyMSSProcessorImpl::getStateInformation (juce::MemoryBlock& destData)
{
    auto obj = std::make_unique<juce::DynamicObject>();
    obj->setProperty ("model", selectedModel);
    obj->setProperty ("batch_size", params.batchSize);
    obj->setProperty ("overlap_size", params.overlapSize);
    obj->setProperty ("chunk_size", params.chunkSize);
    obj->setProperty ("normalize", params.normalize);
    destData.setSize (0);
    juce::MemoryOutputStream (destData, true).writeText (
        juce::JSON::toString (juce::var (obj.release()), true), false, false, nullptr);
}

void PyMSSProcessorImpl::setStateInformation (const void* data, int size)
{
    if (data == nullptr || size <= 0)
        return;

    juce::String text (static_cast<const char*> (data), (size_t) size);
    if (auto parsed = juce::JSON::parse (text); parsed.isObject())
    {
        selectedModel = parsed.getProperty ("model", "").toString();
        params.batchSize = (int) parsed.getProperty ("batch_size", 0);
        params.overlapSize = (int) parsed.getProperty ("overlap_size", 0);
        params.chunkSize = (int) parsed.getProperty ("chunk_size", 0);
        params.normalize = (bool) parsed.getProperty ("normalize", false);
    }
}

//==============================================================================
juce::AudioProcessorEditor* PyMSSProcessorImpl::createEditor()
{
    return new PyMSSAudioProcessorEditor (*this);
}

juce::AudioProcessorEditor* PyMSSAudioProcessor::createEditor()
{
    return new PyMSSAudioProcessorEditor (*this);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PyMSSAudioProcessor();
}

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<PyMSSDocumentController>();
}
