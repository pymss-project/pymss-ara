#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PyMSSDocumentController.h"
#include "Stems.h"

/** Helper accessors for the shared machinery living in the document controller.
    Returns nullptr if the plugin is not bound to ARA. */
class PyMSSProcessorImpl : public juce::AudioProcessor,
                           private juce::AudioProcessorARAExtension
{
public:
    PyMSSProcessorImpl();
    ~PyMSSProcessorImpl() override = default;

    //----------------------------------------------------------------------
    void prepareToPlay (double, int) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "PyMSS ARA Plugin"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorARAExtension* getARAClientExtensions() override { return this; }

    //----------------------------------------------------------------------
    // Shared state (persisted with the DAW project).
    juce::String getSelectedModel() const      { return selectedModel; }
    void setSelectedModel (const juce::String& m) { selectedModel = m; }
    SeparationParams getParams() const         { return params; }
    void setParams (const SeparationParams& p) { params = p; }

    /** Access the document controller's shared machinery (nullptr if not ARA). */
    PyMSSDocumentController* getDC();

    /** Return the first audio source referenced by the current playback regions. */
    juce::ARAAudioSource* getPrimaryAudioSource();

private:
    static juce::AudioProcessor::BusesProperties getBusesProperties();

    PyMSSDocumentController* getDocumentController() const;

    juce::String selectedModel;
    SeparationParams params;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PyMSSProcessorImpl)
};

/** Public AudioProcessor subclass that provides an editor. */
class PyMSSAudioProcessor final : public PyMSSProcessorImpl
{
public:
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
};

//==============================================================================
// JUCE entry points.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();
const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory();
