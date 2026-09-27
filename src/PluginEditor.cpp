#include "PluginEditor.h"
#include "config/Settings.h"

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
// Model download dialog
//==============================================================================
class ModelDownloadPanel final : public juce::Component,
                                 private juce::ListBoxModel
{
public:
    ModelDownloadPanel (const juce::Array<juce::var>& modelsIn,
                        std::function<void (const juce::String&)> onDownloadIn)
        : modelList ("Available models", this), onDownload (std::move (onDownloadIn))
    {
        title.setText ("Models available for download", juce::dontSendNotification);
        title.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, textColour());
        addAndMakeVisible (title);

        searchBox.setTextToShowWhenEmpty ("Search models...", dimTextColour());
        searchBox.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff202126));
        searchBox.setColour (juce::TextEditor::textColourId, textColour());
        searchBox.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff62656d));
        searchBox.onTextChange = [this] { applyFilters(); };
        addAndMakeVisible (searchBox);

        categoryCombo.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff202126));
        categoryCombo.setColour (juce::ComboBox::textColourId, textColour());
        categoryCombo.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff62656d));
        categoryCombo.onChange = [this] { applyFilters(); };
        addAndMakeVisible (categoryCombo);

        modelList.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff202126));
        modelList.setColour (juce::ListBox::outlineColourId, juce::Colour (0xff62656d));
        modelList.setOutlineThickness (1);
        modelList.setRowHeight (42);
        addAndMakeVisible (modelList);

        progressBar.setColour (juce::ProgressBar::backgroundColourId, panelColour());
        progressBar.setColour (juce::ProgressBar::foregroundColourId, accentColour());
        addAndMakeVisible (progressBar);

        status.setColour (juce::Label::textColourId, dimTextColour());
        status.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (status);

        downloadButton.setColour (juce::TextButton::buttonColourId, accentColour());
        downloadButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        downloadButton.onClick = [this] { downloadSelected(); };
        addAndMakeVisible (downloadButton);

        setModels (modelsIn);
        setSize (640, 520);
    }

    void setModels (const juce::Array<juce::var>& catalogModels)
    {
        if (busy)
            return;

        this->allModels.clear();
        for (const auto& model : catalogModels)
            if (! (bool) model.getProperty ("installed", false))
                this->allModels.add (model);

        rebuildCategories();
        applyFilters();
    }

    void setDownloadProgress (const juce::int64 done, const juce::int64 total,
                              const juce::String& message)
    {
        busy = true;
        progress = total > 0 ? juce::jlimit (0.0, 1.0, (double) done / (double) total) : -1.0;
        status.setText (message.isNotEmpty() ? message : "Downloading model...", juce::dontSendNotification);
        modelList.setEnabled (false);
        searchBox.setEnabled (false);
        categoryCombo.setEnabled (false);
        updateButton();
    }

    void setDownloadFinished (const juce::String& modelName, const bool succeeded,
                              const juce::String& message)
    {
        busy = false;
        progress = succeeded ? 1.0 : 0.0;
        modelList.setEnabled (true);
        searchBox.setEnabled (true);
        categoryCombo.setEnabled (true);

        if (succeeded)
            for (int index = allModels.size(); --index >= 0;)
                if (allModels.getReference (index).getProperty ("name", "").toString() == modelName)
                    allModels.remove (index);

        rebuildCategories();
        applyFilters (false);
        status.setText (message, juce::dontSendNotification);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (14);
        title.setBounds (area.removeFromTop (26));
        area.removeFromTop (8);
        auto filters = area.removeFromTop (28);
        categoryCombo.setBounds (filters.removeFromRight (210));
        filters.removeFromRight (8);
        searchBox.setBounds (filters);
        area.removeFromTop (8);
        modelList.setBounds (area.removeFromTop (320));
        area.removeFromTop (10);
        progressBar.setBounds (area.removeFromTop (20));
        area.removeFromTop (4);
        status.setBounds (area.removeFromTop (24));
        downloadButton.setBounds (area.removeFromBottom (30).removeFromRight (120));
    }

    void paint (juce::Graphics& graphics) override
    {
        graphics.fillAll (panelColour());
    }

private:
    int getNumRows() override { return models.size(); }

    static juce::String getCategoryGroup (const juce::var& model)
    {
        const auto category = model.getProperty ("category", "").toString();
        if (category.isEmpty())
            return "Uncategorized";
        return category.upToFirstOccurrenceOf ("/", false, false);
    }

    void rebuildCategories()
    {
        const auto previous = categoryCombo.getText();
        juce::StringArray categories;
        for (const auto& model : allModels)
            categories.addIfNotAlreadyThere (getCategoryGroup (model));
        categories.sort (true);

        categoryCombo.clear (juce::dontSendNotification);
        categoryCombo.addItem ("All categories", 1);
        int selectedId = 1;
        for (int index = 0; index < categories.size(); ++index)
        {
            const int id = index + 2;
            categoryCombo.addItem (categories[index], id);
            if (categories[index] == previous)
                selectedId = id;
        }
        categoryCombo.setSelectedId (selectedId, juce::dontSendNotification);
    }

    void applyFilters (const bool updateStatus = true)
    {
        const auto query = searchBox.getText().trim();
        const auto selectedCategory = categoryCombo.getSelectedId() > 1
                                        ? categoryCombo.getText() : juce::String();

        models.clear();
        for (const auto& model : allModels)
        {
            if (selectedCategory.isNotEmpty() && getCategoryGroup (model) != selectedCategory)
                continue;

            juce::String searchable = model.getProperty ("name", "").toString();
            searchable << " " << model.getProperty ("category", "").toString()
                       << " " << model.getProperty ("architecture", "").toString()
                       << " " << model.getProperty ("target_stem", "").toString();
            if (query.isNotEmpty() && ! searchable.containsIgnoreCase (query))
                continue;
            models.add (model);
        }

        modelList.deselectAllRows();
        modelList.updateContent();
        if (! models.isEmpty())
            modelList.selectRow (0);

        if (updateStatus)
        {
            juce::String text;
            if (allModels.isEmpty())
                text = "All supported models are installed.";
            else if (models.isEmpty())
                text = "No models match the current filters.";
            else if (models.size() == allModels.size())
                text = juce::String (allModels.size()) + " models available.";
            else
                text = juce::String (models.size()) + " of "
                     + juce::String (allModels.size()) + " models shown.";
            status.setText (text, juce::dontSendNotification);
        }
        updateButton();
    }

    void paintListBoxItem (int rowNumber, juce::Graphics& graphics,
                           int width, int height, bool rowIsSelected) override
    {
        if (rowNumber < 0 || rowNumber >= models.size())
            return;

        if (rowIsSelected)
            graphics.fillAll (accentColour().withAlpha (0.22f));

        const auto& model = models.getReference (rowNumber);
        const auto name = model.getProperty ("name", "").toString();
        const auto category = model.getProperty ("category", "").toString();
        const auto sizeBytes = (juce::int64) model.getProperty ("size_bytes", 0);
        juce::String detail = category;
        if (sizeBytes > 0)
            detail << (detail.isNotEmpty() ? "  |  " : "")
                   << juce::String ((double) sizeBytes / (1024.0 * 1024.0), 1) << " MB";

        graphics.setColour (textColour());
        graphics.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        graphics.drawText (name, 8, 2, width - 16, height / 2, juce::Justification::centredLeft);
        graphics.setColour (dimTextColour());
        graphics.setFont (juce::FontOptions (11.0f));
        graphics.drawText (detail, 8, height / 2, width - 16, height / 2 - 2,
                           juce::Justification::centredLeft);
    }

    void selectedRowsChanged (int) override { updateButton(); }
    void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override { downloadSelected(); }

    void downloadSelected()
    {
        const int row = modelList.getSelectedRow();
        if (busy || row < 0 || row >= models.size() || ! onDownload)
            return;

        const auto name = models.getReference (row).getProperty ("name", "").toString();
        if (name.isEmpty())
            return;

        busy = true;
        progress = -1.0;
        status.setText ("Starting download...", juce::dontSendNotification);
        modelList.setEnabled (false);
        searchBox.setEnabled (false);
        categoryCombo.setEnabled (false);
        updateButton();
        onDownload (name);
    }

    void updateButton()
    {
        downloadButton.setEnabled (! busy && modelList.getSelectedRow() >= 0 && ! models.isEmpty());
        downloadButton.setButtonText (busy ? "Downloading..." : "Download");
    }

    juce::Array<juce::var> allModels;
    juce::Array<juce::var> models;
    juce::Label title;
    juce::TextEditor searchBox;
    juce::ComboBox categoryCombo;
    juce::ListBox modelList;
    double progress = 0.0;
    juce::ProgressBar progressBar { progress };
    juce::Label status;
    juce::TextButton downloadButton { "Download" };
    bool busy = false;
    std::function<void (const juce::String&)> onDownload;
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

    modelsButton.setColour (juce::TextButton::buttonColourId, panelColour());
    modelsButton.setColour (juce::TextButton::textColourOffId, textColour());
    modelsButton.onClick = [this] { openModelDownloadDialog(); };
    addAndMakeVisible (modelsButton);

    modelLabel.setColour (juce::Label::textColourId, textColour());
    addAndMakeVisible (modelLabel);
    modelCombo.onChange = [this] { onModelChanged (true); };
    addAndMakeVisible (modelCombo);

    introLabel.setColour (juce::Label::textColourId, textColour());
    addAndMakeVisible (introLabel);
    introBox.setMultiLine (true);
    introBox.setReadOnly (true);
    introBox.setColour (juce::TextEditor::backgroundColourId, panelColour());
    introBox.setColour (juce::TextEditor::textColourId, textColour());
    introBox.setText ("Select a model to see its description.");
    addAndMakeVisible (introBox);

    for (auto* l : { &batchLabel, &overlapLabel, &chunkLabel, &windowLabel,
                     &aggressionLabel, &postProcessThresholdLabel })
    {
        l->setColour (juce::Label::textColourId, textColour());
        addAndMakeVisible (*l);
    }
    batchLabel.setText ("batch_size", juce::dontSendNotification);
    overlapLabel.setText ("overlap_size", juce::dontSendNotification);
    chunkLabel.setText ("chunk_size", juce::dontSendNotification);
    windowLabel.setText ("window_size", juce::dontSendNotification);
    aggressionLabel.setText ("aggression", juce::dontSendNotification);
    postProcessThresholdLabel.setText ("post_threshold", juce::dontSendNotification);

    for (auto* e : { &batchEdit, &overlapEdit, &chunkEdit, &windowEdit, &aggressionEdit })
    {
        e->setInputRestrictions (8, "0123456789");
        e->setText ("0");
        addAndMakeVisible (*e);
    }
    aggressionEdit.setInputRestrictions (3, "0123456789");
    postProcessThresholdEdit.setInputRestrictions (5, "0123456789.");
    postProcessThresholdEdit.setText ("0.2");
    addAndMakeVisible (postProcessThresholdEdit);
    windowEdit.setTooltip ("VR inference window size");
    aggressionEdit.setTooltip ("VR mask aggression from 0 to 100");
    postProcessThresholdEdit.setTooltip ("VR post-process threshold from 0 to 1");

    for (auto* toggle : { &enableTtaToggle, &standardizeToggle, &highEndProcessToggle,
                          &enablePostProcessToggle, &normalizeToggle })
    {
        toggle->setColour (juce::ToggleButton::textColourId, textColour());
        toggle->setClickingTogglesState (true);
        addAndMakeVisible (*toggle);
    }
    enableTtaToggle.setTooltip ("Test-time augmentation; improves some models but takes longer");
    standardizeToggle.setTooltip ("Standardize the input mix before MSS inference");
    highEndProcessToggle.setTooltip ("Reconstruct high frequencies for VR models");
    enablePostProcessToggle.setTooltip ("Apply VR mask artifact post-processing");
    normalizeToggle.setTooltip ("Peak-normalize the separated output stems");

    auto persistParameterEdits = [this] { processor.setParams (gatherParams()); };
    for (auto* editor : { &batchEdit, &overlapEdit, &chunkEdit, &windowEdit,
                          &aggressionEdit, &postProcessThresholdEdit })
        editor->onTextChange = persistParameterEdits;
    for (auto* toggle : { &enableTtaToggle, &standardizeToggle, &highEndProcessToggle,
                          &enablePostProcessToggle, &normalizeToggle })
        toggle->onClick = persistParameterEdits;

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

    // Restore saved params and select the matching architecture-specific panel.
    // The first model-info response must not replace project state with catalog defaults.
    preserveParamsOnInitialSelection = processor.getSelectedModel().isNotEmpty();
    setInferenceParams (processor.getParams());

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
    top.removeFromRight (8);
    modelsButton.setBounds (top.removeFromRight (90).withSizeKeepingCentre (90, 28));
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

    // Architecture-specific inference parameters (2 columns x 4 rows).
    {
        auto paramArea = r.removeFromTop (124);
        auto left = paramArea.removeFromLeft (paramArea.getWidth() / 2);
        auto right = paramArea;
        auto nextRow = [] (juce::Rectangle<int>& column)
        {
            auto row = column.removeFromTop (26);
            column.removeFromTop (6);
            return row;
        };
        auto setEditorRow = [] (juce::Rectangle<int> row, juce::Label& label,
                                juce::TextEditor& editor, const int labelWidth = 105)
        {
            label.setBounds (row.removeFromLeft (labelWidth));
            editor.setBounds (row);
        };

        setEditorRow (nextRow (left), batchLabel, batchEdit);
        if (currentModelIsVr)
        {
            setEditorRow (nextRow (right), windowLabel, windowEdit);
            setEditorRow (nextRow (left), aggressionLabel, aggressionEdit);
            setEditorRow (nextRow (right), postProcessThresholdLabel, postProcessThresholdEdit, 112);
            enableTtaToggle.setBounds (nextRow (left));
            highEndProcessToggle.setBounds (nextRow (right));
            enablePostProcessToggle.setBounds (nextRow (left));
            normalizeToggle.setBounds (nextRow (right));
        }
        else
        {
            setEditorRow (nextRow (right), chunkLabel, chunkEdit);
            setEditorRow (nextRow (left), overlapLabel, overlapEdit);
            normalizeToggle.setBounds (nextRow (right));
            enableTtaToggle.setBounds (nextRow (left));
            standardizeToggle.setBounds (nextRow (right));
        }
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
    modelSelectionInitialized = false;
    preserveParamsOnInitialSelection = false;
    modelInfoLoading = false;
    lastModelInfoTag = -1;
    {
        juce::ScopedLock sl (cacheLock);
        cachedModels.clear();
        hasCachedModels = false;
        modelsNeedRefresh = false;
    }

    // Restart the worker so the new python_path / model_path take effect, then
    // refresh the model list (the installed set depends on model_path).
    dc->stopWorker();
    dc->startWorker();
    refreshModelList();
}

void PyMSSAudioProcessorEditor::openModelDownloadDialog()
{
    if (dc == nullptr || downloadBusy || ! dc->getWorker().isReady() || ! hasCachedModels.load())
        return;

    juce::Array<juce::var> modelsCopy;
    {
        juce::ScopedLock sl (cacheLock);
        modelsCopy = cachedModels;
    }

    auto safeEditor = juce::Component::SafePointer<PyMSSAudioProcessorEditor> (this);
    auto content = std::make_unique<ModelDownloadPanel> (modelsCopy,
        [safeEditor] (const juce::String& modelName)
        {
            if (auto* editor = safeEditor.getComponent())
                editor->startModelDownload (modelName);
        });
    modelDownloadPanel = content.get();

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "PyMSS Model Downloads";
    options.content.setOwned (content.release());
    options.content->setSize (640, 520);
    options.componentToCentreAround = this;
    options.dialogBackgroundColour = panelColour();
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    modelDownloadDialog = options.launchAsync();
}

void PyMSSAudioProcessorEditor::startModelDownload (const juce::String& modelName)
{
    if (dc == nullptr || downloadBusy || modelName.isEmpty())
        return;

    const auto settings = dc->loadSettings();
    const int tag = dc->getWorker().reserveRequestTag();
    activeDownloadTag = tag;
    downloadBusy = true;
    {
        juce::ScopedLock sl (cacheLock);
        cachedDownloadModel = modelName;
    }
    if (! dc->getWorker().requestModelDownload (tag, modelName, settings.modelPath))
    {
        activeDownloadTag = -1;
        downloadBusy = false;
        if (auto* panel = dynamic_cast<ModelDownloadPanel*> (modelDownloadPanel.getComponent()))
            panel->setDownloadFinished (modelName, false, "Could not start the model download.");
        updateStartButtonText();
        return;
    }

    updateStartButtonText();
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
    modelCombo.clear (juce::dontSendNotification);
    if (dc == nullptr)
        return;

    juce::String restored = processor.getSelectedModel();
    int selectId = 0;
    int firstInstalledId = 0;

    for (int i = 0; i < models.size(); ++i)
    {
        const auto& m = models.getReference (i);
        const int id = i + 1;
        const auto name = m.getProperty ("name", "").toString();
        const bool installed = (bool) m.getProperty ("installed", false);

        if (! installed)
            continue;

        modelCombo.addItem (name, id);
        if (firstInstalledId == 0)
            firstInstalledId = id;

        if (restored.isNotEmpty() && name == restored)
            selectId = id;
    }

    if (selectId > 0)
    {
        modelCombo.setSelectedId (selectId, juce::dontSendNotification);
        if (! modelSelectionInitialized)
        {
            const bool applyDefaults = ! preserveParamsOnInitialSelection;
            preserveParamsOnInitialSelection = false;
            onModelChanged (applyDefaults);
        }
    }
    else if (firstInstalledId > 0)
    {
        preserveParamsOnInitialSelection = false;
        modelCombo.setSelectedId (firstInstalledId, juce::sendNotificationSync);
    }
    else
    {
        modelSelectionInitialized = false;
        preserveParamsOnInitialSelection = false;
        modelInfoLoading = false;
        lastModelInfoTag = -1;
        processor.setSelectedModel ({});
        setInferenceParams ({});
        introBox.setText ("No installed models. Use Models... to download one.");
    }
}

void PyMSSAudioProcessorEditor::onModelChanged (const bool applyDefaults)
{
    const int id = modelCombo.getSelectedId();
    if (id <= 0 || dc == nullptr)
        return;

    juce::var model;
    {
        juce::ScopedLock sl (cacheLock);
        if (id > cachedModels.size())
            return;
        model = cachedModels.getReference (id - 1);
    }

    const auto name = model.getProperty ("name", "").toString();
    currentModelIsVr = model.getProperty ("architecture", "").toString().equalsIgnoreCase ("vr");
    updateParameterControls (currentModelIsVr);
    modelSelectionInitialized = true;
    processor.setSelectedModel (name);
    if (! applyDefaults)
    {
        auto restoredParams = processor.getParams();
        restoredParams.isVrModel = currentModelIsVr;
        processor.setParams (restoredParams);
    }
    if (! (bool) model.getProperty ("installed", false))
        setInferenceParams ({});
    introBox.setText ("Loading model information...");
    requestModelInfo (name, applyDefaults);
}

void PyMSSAudioProcessorEditor::requestModelInfo (const juce::String& modelName,
                                                  const bool applyDefaults)
{
    if (dc == nullptr || modelName.isEmpty())
    {
        modelInfoLoading = false;
        return;
    }

    const auto settings = dc->loadSettings();
    const int tag = dc->getWorker().reserveRequestTag();
    lastModelInfoTag = tag;
    modelInfoShouldApplyDefaults = applyDefaults;
    modelInfoLoading = true;
    updateStartButtonText();
    if (! dc->getWorker().requestModelInfo (tag, modelName, settings.modelPath))
    {
        lastModelInfoTag = -1;
        modelInfoLoading = false;
        introBox.setText ("Model information is unavailable because the worker is not ready.");
        updateStartButtonText();
    }
}

void PyMSSAudioProcessorEditor::setInferenceParams (const SeparationParams& params)
{
    currentModelIsVr = params.isVrModel;
    batchEdit.setText (juce::String (params.batchSize), false);
    overlapEdit.setText (juce::String (params.overlapSize), false);
    chunkEdit.setText (juce::String (params.chunkSize), false);
    windowEdit.setText (juce::String (params.windowSize), false);
    aggressionEdit.setText (juce::String (params.aggression), false);
    auto thresholdText = juce::String (params.postProcessThreshold, 3).trimCharactersAtEnd ("0");
    if (thresholdText.endsWithChar ('.'))
        thresholdText << "0";
    postProcessThresholdEdit.setText (thresholdText, false);
    enableTtaToggle.setToggleState (params.enableTta, juce::dontSendNotification);
    standardizeToggle.setToggleState (params.standardize, juce::dontSendNotification);
    highEndProcessToggle.setToggleState (params.highEndProcess, juce::dontSendNotification);
    enablePostProcessToggle.setToggleState (params.enablePostProcess, juce::dontSendNotification);
    normalizeToggle.setToggleState (params.normalize, juce::dontSendNotification);
    processor.setParams (params);
    updateParameterControls (params.isVrModel);
}

void PyMSSAudioProcessorEditor::updateParameterControls (const bool isVrModel)
{
    currentModelIsVr = isVrModel;

    juce::Component* mssComponents[] = {
        &overlapLabel, &overlapEdit, &chunkLabel, &chunkEdit, &standardizeToggle
    };
    for (auto* component : mssComponents)
        component->setVisible (! isVrModel);

    juce::Component* vrComponents[] = {
        &windowLabel, &windowEdit, &aggressionLabel, &aggressionEdit,
        &postProcessThresholdLabel, &postProcessThresholdEdit,
        &highEndProcessToggle, &enablePostProcessToggle
    };
    for (auto* component : vrComponents)
        component->setVisible (isVrModel);

    paramsHint.setText (isVrModel
                            ? "VR parameters · values loaded from model defaults"
                            : "MSS parameters · 0 = use model default",
                        juce::dontSendNotification);
    resized();
}

void PyMSSAudioProcessorEditor::applyModelInfo (const juce::var& info,
                                                const bool applyDefaults)
{
    const auto name = info.getProperty ("name", "").toString();
    if (name.isNotEmpty() && name != processor.getSelectedModel())
        return;

    modelInfoLoading = false;
    introBox.setText (info.getProperty ("intro", "").toString());
    const bool isVrModel = info.getProperty ("architecture", "").toString().equalsIgnoreCase ("vr");
    if (! applyDefaults)
    {
        auto restoredParams = processor.getParams();
        restoredParams.isVrModel = isVrModel;
        setInferenceParams (restoredParams);
        return;
    }

    SeparationParams params;
    params.isVrModel = isVrModel;
    if ((bool) info.getProperty ("installed", false))
    {
        const auto defaults = info.getProperty ("default_params", juce::var());
        if (defaults.isObject())
        {
            params.batchSize = (int) defaults.getProperty ("batch_size", 0);
            params.overlapSize = (int) defaults.getProperty ("overlap_size", 0);
            params.chunkSize = (int) defaults.getProperty ("chunk_size", 0);
            params.windowSize = (int) defaults.getProperty ("window_size", 512);
            params.aggression = (int) defaults.getProperty ("aggression", 5);
            params.postProcessThreshold = (double) defaults.getProperty ("post_process_threshold", 0.2);
            params.enableTta = (bool) defaults.getProperty ("enable_tta", false);
            params.standardize = (bool) defaults.getProperty ("standardize", false);
            params.highEndProcess = (bool) defaults.getProperty ("high_end_process", false);
            params.enablePostProcess = (bool) defaults.getProperty ("enable_post_process", false);
            params.normalize = (bool) defaults.getProperty ("normalize", false);
        }
    }
    setInferenceParams (params);
}

bool PyMSSAudioProcessorEditor::isModelInstalled (const juce::String& modelName)
{
    juce::ScopedLock sl (cacheLock);
    for (const auto& model : cachedModels)
        if (model.getProperty ("name", "").toString() == modelName)
            return (bool) model.getProperty ("installed", false);
    return false;
}

//==============================================================================
SeparationParams PyMSSAudioProcessorEditor::gatherParams() const
{
    SeparationParams p;
    p.isVrModel = currentModelIsVr;
    p.batchSize = batchEdit.getText().getIntValue();
    p.overlapSize = overlapEdit.getText().getIntValue();
    p.chunkSize = chunkEdit.getText().getIntValue();
    p.windowSize = windowEdit.getText().getIntValue();
    p.aggression = aggressionEdit.getText().getIntValue();
    p.postProcessThreshold = postProcessThresholdEdit.getText().getDoubleValue();
    p.enableTta = enableTtaToggle.getToggleState();
    p.standardize = standardizeToggle.getToggleState();
    p.highEndProcess = highEndProcessToggle.getToggleState();
    p.enablePostProcess = enablePostProcessToggle.getToggleState();
    p.normalize = normalizeToggle.getToggleState();
    return p;
}

void PyMSSAudioProcessorEditor::onStartButtonClicked()
{
    if (dc == nullptr || downloadBusy)
        return;

    auto& engine = dc->getEngine();

    if (engine.isBusy())
    {
        engine.cancel();
        return;
    }

    if (modelInfoLoading)
        return;

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
    const auto thresholdText = postProcessThresholdEdit.getText().trim();
    if (params.isVrModel
        && (thresholdText.isEmpty()
            || ! thresholdText.containsAnyOf ("0123456789")
            || thresholdText.indexOfChar ('.') != thresholdText.lastIndexOfChar ('.')
            || params.aggression < 0 || params.aggression > 100
            || params.postProcessThreshold < 0.0 || params.postProcessThreshold > 1.0))
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Invalid VR parameters",
            "aggression must be between 0 and 100, and post_threshold must be between 0 and 1.");
        return;
    }
    processor.setParams (params);

    const auto settings = dc->loadSettings();
    const auto sourceId = source->getPersistentID();
    auto* dcPtr = dc;

    pendingDefaultsRefreshModel = isModelInstalled (model) ? juce::String() : model;

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
    settingsButton.setEnabled (! engine.isBusy() && ! downloadBusy);
    modelsButton.setEnabled (! engine.isBusy() && ! downloadBusy
                             && dc->getWorker().isReady() && hasCachedModels.load());
    const bool modelSelectionEnabled = ! engine.isBusy() && ! downloadBusy;
    const bool parameterEditingEnabled = modelSelectionEnabled && ! modelInfoLoading;
    modelCombo.setEnabled (modelSelectionEnabled);
    juce::Component* parameterControls[] = {
        &batchEdit, &overlapEdit, &chunkEdit, &windowEdit, &aggressionEdit,
        &postProcessThresholdEdit, &enableTtaToggle, &standardizeToggle,
        &highEndProcessToggle, &enablePostProcessToggle, &normalizeToggle
    };
    for (auto* component : parameterControls)
        component->setEnabled (parameterEditingEnabled);
    if (engine.getState() == SeparationEngine::State::restarting)
    {
        startButton.setButtonText ("Restarting...");
        startButton.setEnabled (false);
    }
    else if (downloadBusy)
    {
        startButton.setButtonText ("Downloading model...");
        startButton.setEnabled (false);
    }
    else if (! dc->getWorker().isReady())
    {
        startButton.setButtonText ("Worker unavailable");
        startButton.setEnabled (false);
    }
    else if (engine.isBusy())
    {
        startButton.setButtonText ("Cancel");
        startButton.setEnabled (true);
    }
    else if (modelInfoLoading)
    {
        startButton.setButtonText ("Loading model...");
        startButton.setEnabled (false);
    }
    else if (engine.getState() == SeparationEngine::State::done
             || (processor.getPrimaryAudioSource() != nullptr
                 && engine.hasStemsForSource (processor.getPrimaryAudioSource()->getPersistentID())))
    {
        startButton.setButtonText ("Re-separate");
        startButton.setEnabled (true);
    }
    else
    {
        startButton.setButtonText ("Start Separation");
        startButton.setEnabled (true);
    }
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
        case SeparationEngine::State::restarting:  text = "Restarting Python worker..."; break;
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
    bool applyModelInfoResult = false;
    int modelInfoTag = -1;
    juce::var modelInfo;
    juce::String modelInfoError;
    bool applyDownloadProgress = false;
    bool applyDownloadFinished = false;
    juce::int64 downloadDone = 0;
    juce::int64 downloadTotal = 0;
    juce::String downloadMessage;
    juce::String downloadedModel;
    juce::String downloadError;
    juce::var downloadedInfo;
    bool downloadSucceeded = false;
    {
        juce::ScopedLock sl (cacheLock);
        refreshModels = modelsNeedRefresh;
        modelsNeedRefresh = false;
        applyPymss = pymssNeedsApply;
        pymssNeedsApply = false;
        applyModelInfoResult = modelInfoNeedsApply;
        modelInfoNeedsApply = false;
        modelInfoTag = cachedModelInfoTag;
        modelInfo = cachedModelInfo;
        modelInfoError = cachedModelInfoError;
        applyDownloadProgress = downloadProgressNeedsApply;
        downloadProgressNeedsApply = false;
        applyDownloadFinished = downloadFinishedNeedsApply;
        downloadFinishedNeedsApply = false;
        downloadDone = cachedDownloadDone;
        downloadTotal = cachedDownloadTotal;
        downloadMessage = cachedDownloadMessage;
        downloadedModel = cachedDownloadModel;
        downloadError = cachedDownloadError;
        downloadedInfo = cachedDownloadInfo;
        downloadSucceeded = cachedDownloadSucceeded;
    }

    if (refreshModels)
    {
        juce::Array<juce::var> modelsCopy;
        {
            juce::ScopedLock sl (cacheLock);
            modelsCopy = cachedModels;
        }
        populateModelCombo (modelsCopy);
        if (auto* panel = dynamic_cast<ModelDownloadPanel*> (modelDownloadPanel.getComponent()))
            panel->setModels (modelsCopy);
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
            modelInfoLoading = false;
            lastModelInfoTag = -1;
            // pymss missing takes priority over the model intro.
            introBox.setText ("pymss dependency not found in the selected Python environment. "
                              "Please make sure pymss is installed.\n\n" + msg);
        }
        else if (processor.getSelectedModel().isEmpty())
        {
            introBox.setText (hasCachedModels.load() && modelCombo.getNumItems() == 0
                                  ? "No installed models. Use Models... to download one."
                                  : "pymss detected (" + msg + "). Select a model to see its description.");
        }
    }

    if (applyModelInfoResult && modelInfoTag == lastModelInfoTag.load()
        && dc != nullptr && dc->getWorker().isReady())
    {
        if (modelInfoError.isEmpty())
            applyModelInfo (modelInfo, modelInfoShouldApplyDefaults);
        else
        {
            modelInfoLoading = false;
            introBox.setText ("Could not load model information.\n\n" + modelInfoError);
            if (modelInfoShouldApplyDefaults)
            {
                SeparationParams fallbackParams;
                fallbackParams.isVrModel = currentModelIsVr;
                setInferenceParams (fallbackParams);
            }
        }
    }

    if (applyDownloadProgress)
        if (auto* panel = dynamic_cast<ModelDownloadPanel*> (modelDownloadPanel.getComponent()))
            panel->setDownloadProgress (downloadDone, downloadTotal, downloadMessage);

    if (applyDownloadFinished)
    {
        downloadBusy = false;
        if (auto* panel = dynamic_cast<ModelDownloadPanel*> (modelDownloadPanel.getComponent()))
            panel->setDownloadFinished (
                downloadedModel,
                downloadSucceeded,
                downloadSucceeded ? "Download complete." : "Download failed: " + downloadError);

        if (downloadSucceeded)
        {
            processor.setSelectedModel (downloadedModel);
            modelSelectionInitialized = true;
            applyModelInfo (downloadedInfo, true);
            refreshModelList();
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
    if (tag != lastModelInfoTag.load())
        return;

    juce::ScopedLock sl (cacheLock);
    cachedModelInfo = info;
    cachedModelInfoError.clear();
    cachedModelInfoTag = tag;
    modelInfoNeedsApply = true;
}

void PyMSSAudioProcessorEditor::modelInfoFailed (int tag, const juce::String& message)
{
    if (tag != lastModelInfoTag.load())
        return;

    juce::ScopedLock sl (cacheLock);
    cachedModelInfo = juce::var();
    cachedModelInfoError = message;
    cachedModelInfoTag = tag;
    modelInfoNeedsApply = true;
}

void PyMSSAudioProcessorEditor::modelDownloadProgress (int tag, juce::int64 done,
                                                       juce::int64 total,
                                                       const juce::String& message)
{
    if (tag != activeDownloadTag.load())
        return;
    juce::ScopedLock sl (cacheLock);
    cachedDownloadDone = done;
    cachedDownloadTotal = total;
    cachedDownloadMessage = message;
    downloadProgressNeedsApply = true;
}

void PyMSSAudioProcessorEditor::modelDownloadDone (int tag, const juce::String& modelName,
                                                   const juce::var& info)
{
    int expectedTag = tag;
    if (! activeDownloadTag.compare_exchange_strong (expectedTag, -1))
        return;
    juce::ScopedLock sl (cacheLock);
    cachedDownloadModel = modelName;
    cachedDownloadInfo = info;
    cachedDownloadError.clear();
    cachedDownloadSucceeded = true;
    downloadFinishedNeedsApply = true;
}

void PyMSSAudioProcessorEditor::modelDownloadFailed (int tag, const juce::String& message)
{
    int expectedTag = tag;
    if (! activeDownloadTag.compare_exchange_strong (expectedTag, -1))
        return;
    juce::ScopedLock sl (cacheLock);
    cachedDownloadError = message;
    cachedDownloadSucceeded = false;
    downloadFinishedNeedsApply = true;
}

void PyMSSAudioProcessorEditor::workerDied (const juce::String& reason)
{
    juce::ScopedLock sl (cacheLock);
    cachedPymssOk = false;
    cachedPymssMessage = "Worker process is not running: " + reason;
    pymssNeedsApply = true;
    if (activeDownloadTag.exchange (-1) >= 0)
    {
        cachedDownloadError = reason;
        cachedDownloadSucceeded = false;
        downloadFinishedNeedsApply = true;
    }
}

//==============================================================================
void PyMSSAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (dc != nullptr)
    {
        const auto state = dc->getEngine().getState();
        if (state == SeparationEngine::State::done)
            rebuildStemRows();
        if (pendingDefaultsRefreshModel.isNotEmpty()
            && (state == SeparationEngine::State::done
                || state == SeparationEngine::State::failed
                || state == SeparationEngine::State::cancelled))
        {
            const auto modelName = pendingDefaultsRefreshModel;
            pendingDefaultsRefreshModel.clear();
            if (dc->getWorker().isReady())
            {
                requestModelInfo (modelName);
                refreshModelList();
            }
            else
            {
                modelSelectionInitialized = false;
            }
        }
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
