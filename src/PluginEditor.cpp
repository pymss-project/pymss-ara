#include "PluginEditor.h"
#include "Settings.h"

//==============================================================================
namespace
{
juce::Colour backgroundColour()    { return juce::Colour (0xff2b2b30); }
juce::Colour panelColour()         { return juce::Colour (0xff34343c); }
juce::Colour accentColour()        { return juce::Colour (0xff5b9dff); }
juce::Colour textColour()          { return juce::Colour (0xffe6e6ec); }
juce::Colour dimTextColour()       { return juce::Colour (0xff9a9aa3); }
} // namespace

//==============================================================================
// Settings dialog
//==============================================================================
class SettingsPanel : public juce::Component,
                      private juce::Button::Listener
{
public:
    SettingsPanel (const AraSettings& initial, std::function<void (const juce::String&, const juce::String&)> onSaveIn)
        : onSave (std::move (onSaveIn))
    {
        addAndMakeVisible (pythonLabel);   pythonLabel.setText ("python_path", juce::dontSendNotification);
        addAndMakeVisible (pythonEdit);    pythonEdit.setText (initial.pythonPath);
        addAndMakeVisible (pythonBrowse);  pythonBrowse.setButtonText ("Browse...");
        addAndMakeVisible (modelLabel);    modelLabel.setText ("model_path", juce::dontSendNotification);
        addAndMakeVisible (modelEdit);     modelEdit.setText (initial.modelPath);
        addAndMakeVisible (modelBrowse);   modelBrowse.setButtonText ("Browse...");
        addAndMakeVisible (saveButton);    saveButton.setButtonText ("Save");
        addAndMakeVisible (hintLabel);     hintLabel.setText (
            "Leave python_path empty to use the system \"python\". Leave model_path empty to use the pymss default model directory.",
            juce::dontSendNotification);
        hintLabel.setColour (juce::Label::textColourId, dimTextColour());
        hintLabel.setFont (juce::FontOptions (13.0f));

        pythonBrowse.addListener (this);
        modelBrowse.addListener (this);
        saveButton.addListener (this);

        for (auto* c : { &pythonLabel, &modelLabel })
            c->setColour (juce::Label::textColourId, textColour());

        setSize (560, 200);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        auto row = r.removeFromTop (28);
        pythonLabel.setBounds (row.removeFromLeft (90));
        pythonEdit.setBounds (row.removeFromLeft (row.getWidth() - 100));
        pythonBrowse.setBounds (row);
        r.removeFromTop (8);

        row = r.removeFromTop (28);
        modelLabel.setBounds (row.removeFromLeft (90));
        modelEdit.setBounds (row.removeFromLeft (row.getWidth() - 100));
        modelBrowse.setBounds (row);
        r.removeFromTop (8);

        hintLabel.setBounds (r.removeFromTop (44));
        r.removeFromTop (6);
        saveButton.setBounds (r.removeFromTop (28).removeFromRight (100));
    }

    void paint (juce::Graphics& g) override { g.fillAll (panelColour()); }

private:
    void buttonClicked (juce::Button* b) override
    {
        if (b == &pythonBrowse)
        {
            juce::FileChooser fc ("Select Python interpreter", juce::File (pythonEdit.getText()), "*.exe");
            if (fc.browseForFileToOpen())
                pythonEdit.setText (fc.getResult().getFullPathName());
        }
        else if (b == &modelBrowse)
        {
            juce::FileChooser fc ("Select model folder", juce::File (modelEdit.getText()));
            if (fc.browseForDirectory())
                modelEdit.setText (fc.getResult().getFullPathName());
        }
        else if (b == &saveButton)
        {
            if (onSave)
                onSave (pythonEdit.getText().trim(), modelEdit.getText().trim());
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState (0);
        }
    }

    juce::Label pythonLabel, modelLabel, hintLabel;
    juce::TextEditor pythonEdit, modelEdit;
    juce::TextButton pythonBrowse, modelBrowse, saveButton;
    std::function<void (const juce::String&, const juce::String&)> onSave;
};

//==============================================================================
StemMixRow::StemMixRow (int stemIndex, const juce::String& name, SeparationEngine& engineRef)
    : index (stemIndex), engine (engineRef)
{
    nameLabel.setText (juce::String (index + 1) + ".  " + name, juce::dontSendNotification);
    nameLabel.setColour (juce::Label::textColourId, textColour());
    addAndMakeVisible (nameLabel);

    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonColourId, panelColour());
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::indianred);
    muteButton.setColour (juce::TextButton::textColourOffId, textColour());
    muteButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    muteButton.setTooltip ("Mute this stem");
    muteButton.setToggleState (engine.isStemMuted (index), juce::dontSendNotification);
    muteButton.onClick = [this] { engine.setStemMuted (index, muteButton.getToggleState()); };
    addAndMakeVisible (muteButton);

    soloButton.setClickingTogglesState (true);
    soloButton.setColour (juce::TextButton::buttonColourId, panelColour());
    soloButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::goldenrod);
    soloButton.setColour (juce::TextButton::textColourOffId, textColour());
    soloButton.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    soloButton.setTooltip ("Solo this stem");
    soloButton.setToggleState (engine.isStemSolo (index), juce::dontSendNotification);
    soloButton.onClick = [this] { engine.setStemSolo (index, soloButton.getToggleState()); };
    addAndMakeVisible (soloButton);
}

void StemMixRow::paint (juce::Graphics& g)
{
    g.fillAll (panelColour());
    // Visual cue: dimmed when the stem is currently inaudible.
    const bool active = engine.isStemActive (index);
    if (! active)
    {
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRect (getLocalBounds());
    }
}

void StemMixRow::resized()
{
    auto r = getLocalBounds().reduced (4);
    soloButton.setBounds (r.removeFromRight (44));
    r.removeFromRight (6);
    muteButton.setBounds (r.removeFromRight (44));
    r.removeFromRight (8);
    nameLabel.setBounds (r);
}

//==============================================================================
PyMSSAudioProcessorEditor::PyMSSAudioProcessorEditor (PyMSSProcessorImpl& p)
    : juce::AudioProcessorEditor (&p),
      juce::AudioProcessorEditorARAExtension (&p),
      processor (p)
{
    dc = processor.getDC();

    titleLabel.setText ("PyMSS ARA Plugin", juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    titleLabel.setJustificationType (juce::Justification::centred);
    titleLabel.setColour (juce::Label::textColourId, textColour());
    addAndMakeVisible (titleLabel);

    settingsButton.setColour (juce::TextButton::buttonColourId, panelColour());
    settingsButton.setColour (juce::TextButton::textColourOffId, textColour());
    settingsButton.onClick = [this] { openSettingsDialog(); };
    addAndMakeVisible (settingsButton);

    modelLabel.setColour (juce::Label::textColourId, textColour());
    addAndMakeVisible (modelLabel);
    modelCombo.onChange = [this] { onModelChanged(); };
    addAndMakeVisible (modelCombo);

    introLabel.setColour (juce::Label::textColourId, textColour());
    addAndMakeVisible (introLabel);
    introBox.setMultiLine (true);
    introBox.setReadOnly (true);
    introBox.setColour (juce::TextEditor::backgroundColourId, panelColour());
    introBox.setColour (juce::TextEditor::textColourId, textColour());
    introBox.setText ("Select a model to see its description.");
    addAndMakeVisible (introBox);

    for (auto* l : { &batchLabel, &overlapLabel, &chunkLabel })
    {
        l->setColour (juce::Label::textColourId, textColour());
        addAndMakeVisible (*l);
    }
    batchLabel.setText ("batch_size", juce::dontSendNotification);
    overlapLabel.setText ("overlap_size", juce::dontSendNotification);
    chunkLabel.setText ("chunk_size", juce::dontSendNotification);

    for (auto* e : { &batchEdit, &overlapEdit, &chunkEdit })
    {
        e->setInputRestrictions (8, "0123456789");
        e->setText ("0");
        addAndMakeVisible (*e);
    }

    normalizeToggle.setColour (juce::ToggleButton::textColourId, textColour());
    normalizeToggle.setClickingTogglesState (true);
    addAndMakeVisible (normalizeToggle);

    paramsHint.setColour (juce::Label::textColourId, dimTextColour());
    paramsHint.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (paramsHint);

    startButton.setColour (juce::TextButton::buttonColourId, accentColour());
    startButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    startButton.onClick = [this] { onStartButtonClicked(); };
    addAndMakeVisible (startButton);

    progressBar.setColour (juce::ProgressBar::backgroundColourId, panelColour());
    progressBar.setColour (juce::ProgressBar::foregroundColourId, accentColour());
    addAndMakeVisible (progressBar);

    statusLabel.setColour (juce::Label::textColourId, dimTextColour());
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (statusLabel);

    stemsLabel.setText ("Stems (Mute / Solo)", juce::dontSendNotification);
    stemsLabel.setColour (juce::Label::textColourId, textColour());
    stemsLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    addAndMakeVisible (stemsLabel);

    // Restore saved params.
    const auto params = processor.getParams();
    batchEdit.setText (juce::String (params.batchSize));
    overlapEdit.setText (juce::String (params.overlapSize));
    chunkEdit.setText (juce::String (params.chunkSize));
    normalizeToggle.setToggleState (params.normalize, juce::dontSendNotification);

    setResizable (true, false);
    setSize (620, 700);

    if (dc != nullptr)
    {
        dc->getWorker().addListener (this);
        dc->getEngine().addChangeListener (this);
        if (! dc->isWorkerRunning())
            dc->startWorker();
        else
            refreshModelList();

        rebuildStemRows(); // show existing stems if reopened after a separation
    }

    startTimerHz (30);
}

PyMSSAudioProcessorEditor::~PyMSSAudioProcessorEditor()
{
    stopTimer();
    if (dc != nullptr)
    {
        dc->getWorker().removeListener (this);
        dc->getEngine().removeChangeListener (this);
    }
}

//==============================================================================
void PyMSSAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour());

    if (dc == nullptr)
    {
        g.setColour (textColour());
        g.setFont (juce::FontOptions (16.0f));
        g.drawFittedText ("ARA host isn't detected. This plugin only supports ARA mode.",
                          getLocalBounds(), juce::Justification::centred, 1);
    }
}

void PyMSSAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto top = r.removeFromTop (34);
    settingsButton.setBounds (top.removeFromRight (90).withSizeKeepingCentre (90, 28));
    titleLabel.setBounds (top);

    r.removeFromTop (12);

    // Model row
    {
        auto row = r.removeFromTop (26);
        modelLabel.setBounds (row.removeFromLeft (70));
        modelCombo.setBounds (row);
    }
    r.removeFromTop (8);

    // Intro
    introLabel.setBounds (r.removeFromTop (20));
    r.removeFromTop (4);
    introBox.setBounds (r.removeFromTop (120));
    r.removeFromTop (10);

    // Params (2 columns x 2 rows)
    {
        auto paramArea = r.removeFromTop (90);
        auto col1 = paramArea.removeFromLeft (paramArea.getWidth() / 2);
        auto col2 = paramArea;

        auto row1 = col1.removeFromTop (26);
        batchLabel.setBounds (row1.removeFromLeft (85));
        batchEdit.setBounds (row1);
        col1.removeFromTop (8);
        auto row2 = col1.removeFromTop (26);
        overlapLabel.setBounds (row2.removeFromLeft (85));
        overlapEdit.setBounds (row2);

        auto c1 = col2.removeFromTop (26);
        chunkLabel.setBounds (c1.removeFromLeft (85));
        chunkEdit.setBounds (c1);
        col2.removeFromTop (8);
        auto c2 = col2.removeFromTop (26);
        normalizeToggle.setBounds (c2);
    }
    paramsHint.setBounds (r.removeFromTop (20));
    r.removeFromTop (8);

    startButton.setBounds (r.removeFromTop (34).withSizeKeepingCentre (220, 34));
    r.removeFromTop (8);
    progressBar.setBounds (r.removeFromTop (22));
    r.removeFromTop (4);
    statusLabel.setBounds (r.removeFromTop (20));
    r.removeFromTop (10);

    // Stem monitor (mute/solo per stem).
    stemsLabel.setBounds (r.removeFromTop (22));
    r.removeFromTop (4);
    for (auto* row : stemRows)
        row->setBounds (r.removeFromTop (26));
}

//==============================================================================
void PyMSSAudioProcessorEditor::openSettingsDialog()
{
    if (dc == nullptr)
        return;

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "PyMSS ARA Settings";
    auto content = std::make_unique<SettingsPanel> (dc->loadSettings(),
        [this] (const juce::String& py, const juce::String& mp) { onSettingsSaved (py, mp); });
    o.content.setOwned (content.release());
    o.content->setSize (560, 200);
    o.componentToCentreAround = this;
    o.dialogBackgroundColour = panelColour();
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = false;
    settingsDialog = o.launchAsync();
}

void PyMSSAudioProcessorEditor::onSettingsSaved (const juce::String& pythonPath, const juce::String& modelPath)
{
    if (dc == nullptr)
        return;

    AraSettings s;
    s.pythonPath = pythonPath;
    s.modelPath = modelPath;
    dc->saveSettings (s);

    // Restart the worker so the new python_path / model_path take effect, then
    // refresh the model list (the installed set depends on model_path).
    dc->stopWorker();
    dc->startWorker();
    refreshModelList();
}

//==============================================================================
void PyMSSAudioProcessorEditor::refreshModelList()
{
    if (dc == nullptr)
        return;

    const auto settings = dc->loadSettings();
    dc->getWorker().checkPymss();
    dc->getWorker().requestModelList (settings.modelPath);
}

void PyMSSAudioProcessorEditor::populateModelCombo (const juce::Array<juce::var>& models)
{
    modelCombo.clear();
    if (dc == nullptr)
        return;

    const auto settings = dc->loadSettings();
    juce::String restored = processor.getSelectedModel();
    int selectId = 0;

    for (int i = 0; i < models.size(); ++i)
    {
        const auto& m = models.getReference (i);
        const int id = i + 1;
        const auto name = m.getProperty ("name", "").toString();
        const bool installed = (bool) m.getProperty ("installed", false);

        juce::String text = name;
        if (! installed)
            text << "   (not installed)";

        modelCombo.addItem (text, id);
        // Uninstalled entries stay selectable so the user can pick them and rely
        // on auto-download (download=True). The "(not installed)" suffix and
        // ordering (installed first) provide the visual distinction.
        juce::ignoreUnused (installed);

        if (restored.isNotEmpty() && name == restored)
            selectId = id;
    }

    if (selectId > 0)
        modelCombo.setSelectedId (selectId, juce::dontSendNotification);
    else if (modelCombo.getNumItems() > 0)
        modelCombo.setSelectedId (1, juce::sendNotificationSync);
}

void PyMSSAudioProcessorEditor::onModelChanged()
{
    const int id = modelCombo.getSelectedId();
    if (id <= 0 || dc == nullptr)
        return;

    const auto& models = cachedModels;
    if (id > models.size())
        return;

    const auto name = models.getReference (id - 1).getProperty ("name", "").toString();
    processor.setSelectedModel (name);

    const auto settings = dc->loadSettings();
    requestModelInfo (name);
    (void) settings;
}

void PyMSSAudioProcessorEditor::requestModelInfo (const juce::String& modelName)
{
    if (dc == nullptr || modelName.isEmpty())
        return;

    const auto settings = dc->loadSettings();
    lastModelInfoTag = dc->getWorker().requestModelInfo (modelName, settings.modelPath);
}

//==============================================================================
SeparationParams PyMSSAudioProcessorEditor::gatherParams() const
{
    SeparationParams p;
    p.batchSize = batchEdit.getText().getIntValue();
    p.overlapSize = overlapEdit.getText().getIntValue();
    p.chunkSize = chunkEdit.getText().getIntValue();
    p.normalize = normalizeToggle.getToggleState();
    return p;
}

void PyMSSAudioProcessorEditor::onStartButtonClicked()
{
    if (dc == nullptr)
        return;

    auto& engine = dc->getEngine();

    if (engine.isBusy())
    {
        engine.cancel();
        return;
    }

    const auto model = processor.getSelectedModel();
    if (model.isEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "No model selected", "Please choose a model first.");
        return;
    }

    auto* source = processor.getPrimaryAudioSource();
    if (source == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                                                "No audio", "Assign an ARA region/track to the plugin first.");
        return;
    }

    const auto params = gatherParams();
    processor.setParams (params);

    const auto settings = dc->loadSettings();
    const auto sourceId = source->getPersistentID();
    auto* dcPtr = dc;

    engine.requestSeparation (
        [dcPtr, source] (juce::AudioBuffer<float>& out, double& nativeSR) -> bool
        {
            return dcPtr != nullptr && dcPtr->readAudioSource (source, out, nativeSR);
        },
        sourceId, model, settings.modelPath, params, 0.0);
}

void PyMSSAudioProcessorEditor::updateStartButtonText()
{
    if (dc == nullptr)
    {
        startButton.setButtonText ("Start Separation");
        return;
    }

    auto& engine = dc->getEngine();
    if (engine.isBusy())
        startButton.setButtonText ("Cancel");
    else if (engine.getState() == SeparationEngine::State::done
             || (processor.getPrimaryAudioSource() != nullptr
                 && engine.hasStemsForSource (processor.getPrimaryAudioSource()->getPersistentID())))
        startButton.setButtonText ("Re-separate");
    else
        startButton.setButtonText ("Start Separation");
}

void PyMSSAudioProcessorEditor::updateProgressDisplay()
{
    if (dc == nullptr)
        return;

    auto& engine = dc->getEngine();
    progressValue = (double) engine.getProgress();

    juce::String text;
    switch (engine.getState())
    {
        case SeparationEngine::State::idle:        text = "Idle"; break;
        case SeparationEngine::State::reading:     text = "Reading audio source..."; break;
        case SeparationEngine::State::downloading: text = "Downloading model..."; break;
        case SeparationEngine::State::separating:  text = engine.getStatusMessage() + "  (" + engine.getProgressText() + ")"; break;
        case SeparationEngine::State::done:        text = "Separation complete"; break;
        case SeparationEngine::State::failed:      text = "Failed: " + engine.getErrorMessage(); break;
        case SeparationEngine::State::cancelled:   text = "Cancelled"; break;
    }
    statusLabel.setText (text, juce::dontSendNotification);

    updateStartButtonText();
}

//==============================================================================
void PyMSSAudioProcessorEditor::timerCallback()
{
    // Apply worker results that arrived on the reader thread.
    bool refreshModels = false;
    bool applyPymss = false;
    {
        juce::ScopedLock sl (cacheLock);
        refreshModels = modelsNeedRefresh;
        modelsNeedRefresh = false;
        applyPymss = pymssNeedsApply;
        pymssNeedsApply = false;
    }

    if (refreshModels)
    {
        juce::Array<juce::var> modelsCopy;
        {
            juce::ScopedLock sl (cacheLock);
            modelsCopy = cachedModels;
        }
        populateModelCombo (modelsCopy);
    }

    if (applyPymss)
    {
        juce::String msg;
        bool ok = false;
        {
            juce::ScopedLock sl (cacheLock);
            msg = cachedPymssMessage;
            ok = cachedPymssOk;
        }

        if (! ok)
        {
            // pymss missing takes priority over the model intro.
            introBox.setText ("pymss dependency not found in the selected Python environment. "
                              "Please make sure pymss is installed.\n\n" + msg);
        }
        else if (processor.getSelectedModel().isEmpty())
        {
            introBox.setText ("pymss detected (" + msg + "). Select a model to see its description.");
        }
    }

    updateProgressDisplay();
}

//==============================================================================
// WorkerClient::Listener (worker reader thread): just cache, apply on the timer.
void PyMSSAudioProcessorEditor::workerReady (bool pymssOk, const juce::String&, const juce::String&)
{
    if (dc != nullptr && pymssOk)
        refreshModelList();
}

void PyMSSAudioProcessorEditor::pymssCheckResult (int, bool ok, const juce::String&, const juce::String& message)
{
    juce::ScopedLock sl (cacheLock);
    cachedPymssOk = ok;
    cachedPymssMessage = message;
    hasPymssCheck = true;
    pymssNeedsApply = true;
}

void PyMSSAudioProcessorEditor::modelListResult (int, const juce::Array<juce::var>& models)
{
    juce::ScopedLock sl (cacheLock);
    cachedModels = models;
    hasCachedModels = true;
    modelsNeedRefresh = true;
}

void PyMSSAudioProcessorEditor::modelInfoResult (int tag, const juce::var& info)
{
    if (tag != lastModelInfoTag)
        return;

    const auto intro = info.getProperty ("intro", "").toString();
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::Component> (this), intro]
    {
        if (auto* self = dynamic_cast<PyMSSAudioProcessorEditor*> (safe.getComponent()))
        {
            // Only override the intro if pymss is present (otherwise the warning
            // shown by the pymss-check path takes priority).
            if (self->dc != nullptr && self->dc->isWorkerRunning())
                self->introBox.setText (intro);
        }
    });
}

void PyMSSAudioProcessorEditor::workerDied (const juce::String& reason)
{
    juce::ScopedLock sl (cacheLock);
    cachedPymssOk = false;
    cachedPymssMessage = "Worker process is not running: " + reason;
    pymssNeedsApply = true;
}

//==============================================================================
void PyMSSAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (dc != nullptr)
    {
        const auto state = dc->getEngine().getState();
        if (state == SeparationEngine::State::done)
            rebuildStemRows();
    }

    updateProgressDisplay();
}

void PyMSSAudioProcessorEditor::rebuildStemRows()
{
    stemRows.clear (true);
    lastBuiltStemSourceId.clear();

    if (dc == nullptr)
        return;

    auto* source = processor.getPrimaryAudioSource();
    if (source == nullptr)
        return;

    const auto sourceId = source->getPersistentID();
    auto stems = dc->getEngine().getStemsForSource (sourceId);
    if (stems == nullptr || stems->getNumStems() == 0)
        return;

    lastBuiltStemSourceId = sourceId;
    const int n = juce::jmin (stems->getNumStems(), 8);
    for (int i = 0; i < n; ++i)
    {
        auto* row = new StemMixRow (i, stems->stems[(size_t) i].name, dc->getEngine());
        addAndMakeVisible (row);
        stemRows.add (row);
    }

    resized();
}
