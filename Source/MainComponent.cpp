#include "MainComponent.h"

#include <cmath>

namespace
{
constexpr auto background = 0xff090b12;
constexpr auto panel = 0xff121725;
constexpr auto paleText = 0xffdce6e8;
constexpr auto quietText = 0xff7f9298;

const std::array<juce::Colour, EcosystemEngine::memoryCount> memoryColours {
    juce::Colour(0xff69d2e7), juce::Colour(0xffa7dbd8),
    juce::Colour(0xffe0e4cc), juce::Colour(0xfff38630),
    juce::Colour(0xffff4e50)
};
}

juce::Font ConnectionLookAndFeel::getComboBoxFont(juce::ComboBox& box)
{
    return juce::FontOptions(juce::jlimit(22.0f, 30.0f,
        static_cast<float>(box.getHeight()) * 0.44f));
}

juce::Font ConnectionLookAndFeel::getPopupMenuFont()
{
    return juce::FontOptions(28.0f);
}

juce::Font ConnectionLookAndFeel::getLabelFont(juce::Label&)
{
    return juce::FontOptions(23.0f);
}

juce::Font ConnectionLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return juce::FontOptions(juce::jlimit(21.0f, 28.0f,
        static_cast<float>(buttonHeight) * 0.42f));
}

void ConnectionLookAndFeel::getIdealPopupMenuItemSize(
    const juce::String& text, bool isSeparator, int standardMenuItemHeight,
    int& idealWidth, int& idealHeight)
{
    LookAndFeel_V4::getIdealPopupMenuItemSize(
        text, isSeparator, juce::jmax(64, standardMenuItemHeight),
        idealWidth, idealHeight);
    idealWidth = juce::jmax(idealWidth, 420);
    if (! isSeparator)
        idealHeight = juce::jmax(idealHeight, 64);
}

void ConnectionLookAndFeel::drawToggleButton(
    juce::Graphics& graphics, juce::ToggleButton& button,
    bool highlighted, bool down)
{
    const auto tickSize = juce::jmin(38.0f,
        static_cast<float>(button.getHeight()) - 8.0f);
    drawTickBox(graphics, button, 4.0f,
                (static_cast<float>(button.getHeight()) - tickSize) * 0.5f,
                tickSize, tickSize, button.getToggleState(), button.isEnabled(),
                highlighted, down);
    graphics.setColour(button.findColour(juce::ToggleButton::textColourId));
    graphics.setFont(juce::FontOptions(23.0f));
    graphics.drawFittedText(button.getButtonText(),
        button.getLocalBounds().withTrimmedLeft(static_cast<int>(tickSize) + 16),
        juce::Justification::centredLeft, 2);
}

class MemoryOrb final : public juce::Component
{
public:
    MemoryOrb(int memoryIndex, juce::String memoryName, juce::Colour memoryColour,
              EcosystemEngine& ecosystem)
        : index(memoryIndex), name(std::move(memoryName)), colour(memoryColour), engine(ecosystem)
    {
        setRepaintsOnMouseActivity(true);
    }

    std::function<void(int)> onSelected;
    bool selected = false;
    double animation = 0.0;

    void paint(juce::Graphics& graphics) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(10.0f);
        const auto centre = bounds.getCentre();
        const auto radius = 0.5f * juce::jmin(bounds.getWidth(), bounds.getHeight());
        const auto phase = engine.getPhase(index);
        const auto recording = engine.isRecording(index);
        const auto material = engine.hasMaterial(index);
        const auto breathing = 1.0f + 0.035f * std::sin(static_cast<float>(animation * 2.0
                                                    + static_cast<double>(index)));

        juce::ColourGradient glow(colour.withAlpha(recording ? 0.48f : 0.25f),
                                  centre.x, centre.y,
                                  colour.withAlpha(0.0f), centre.x + radius, centre.y,
                                  true);
        graphics.setGradientFill(glow);
        graphics.fillEllipse(bounds.withSizeKeepingCentre(radius * 2.0f * breathing,
                                                          radius * 2.0f * breathing));

        graphics.setColour(colour.withAlpha(material || recording ? 0.92f : 0.30f));
        graphics.drawEllipse(bounds.reduced(radius * 0.12f), selected ? 6.0f : 3.0f);
        graphics.setColour(colour.withAlpha(0.18f));
        graphics.drawEllipse(bounds.reduced(radius * 0.27f), 2.0f);

        if (material || recording)
        {
            juce::Path arc;
            arc.addCentredArc(centre.x, centre.y, radius * 0.82f, radius * 0.82f,
                              0.0f, 0.0f,
                              static_cast<float>(juce::MathConstants<double>::twoPi
                                                 * juce::jlimit(0.0, 1.0, phase)), true);
            graphics.setColour(recording ? juce::Colours::white : colour.brighter(0.35f));
            graphics.strokePath(arc, juce::PathStrokeType(selected ? 8.0f : 5.0f,
                                juce::PathStrokeType::curved,
                                juce::PathStrokeType::rounded));
        }

        graphics.setColour(juce::Colour(paleText));
        graphics.setFont(juce::FontOptions(selected ? 27.0f : 23.0f,
                                           juce::Font::bold));
        graphics.drawText(name, bounds.withTrimmedTop(radius * 0.62f),
                          juce::Justification::centredTop, false);

        juce::String detail;
        if (recording)
            detail = "ASCOLTO  " + juce::String(engine.getLengthSeconds(index), 1) + " s";
        else if (material)
            detail = juce::String(engine.getLengthSeconds(index), 1) + " s";
        else
            detail = index < EcosystemEngine::midiMemoryCount
                ? "MIDI " + juce::String(engine.getMidiChannelForMemory(index))
                : "AUDIO 7+8";

        graphics.setColour(recording ? juce::Colours::white : juce::Colour(quietText));
        graphics.setFont(juce::FontOptions(15.0f, juce::Font::plain));
        graphics.drawText(detail, bounds.withTrimmedTop(radius * 1.02f),
                          juce::Justification::centredTop, false);
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        if (onSelected)
            onSelected(index);
    }

private:
    int index;
    juce::String name;
    juce::Colour colour;
    EcosystemEngine& engine;
};

MainComponent::MainComponent()
{
    setOpaque(true);

    titleLabel.setText("COMMENTO", juce::dontSendNotification);
    titleLabel.setFont(juce::FontOptions(34.0f, juce::Font::bold));
    titleLabel.setColour(juce::Label::textColourId, juce::Colour(paleText));
    addAndMakeVisible(titleLabel);

    subtitleLabel.setText("quattro voci e un respiro", juce::dontSendNotification);
    subtitleLabel.setFont(juce::FontOptions(17.0f));
    subtitleLabel.setColour(juce::Label::textColourId, juce::Colour(quietText));
    addAndMakeVisible(subtitleLabel);

    statusLabel.setJustificationType(juce::Justification::centredRight);
    statusLabel.setColour(juce::Label::textColourId, juce::Colour(quietText));
    addAndMakeVisible(statusLabel);

    const std::array<juce::String, EcosystemEngine::memoryCount> names {
        "MEMORIA I", "MEMORIA II", "MEMORIA III", "MEMORIA IV", "RESPIRO"
    };
    for (int index = 0; index < EcosystemEngine::memoryCount; ++index)
    {
        orbs[static_cast<size_t>(index)] = std::make_unique<MemoryOrb>(
            index, names[static_cast<size_t>(index)],
            memoryColours[static_cast<size_t>(index)], engine);
        orbs[static_cast<size_t>(index)]->onSelected = [this](int selected)
        {
            selectMemory(selected);
        };
        addAndMakeVisible(*orbs[static_cast<size_t>(index)]);
    }

    const auto styleButton = [](juce::TextButton& button, juce::Colour colour)
    {
        button.setColour(juce::TextButton::buttonColourId, colour.withAlpha(0.18f));
        button.setColour(juce::TextButton::buttonOnColourId, colour.withAlpha(0.42f));
        button.setColour(juce::TextButton::textColourOffId, colour.brighter(0.7f));
        button.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    };
    styleButton(recordButton, memoryColours[0]);
    styleButton(clearButton, juce::Colour(0xffad496a));
    styleButton(settingsButton, juce::Colour(0xff6a7c91));
    styleButton(model12RoutingButton, juce::Colour(0xff5da8a1));
    addAndMakeVisible(recordButton);
    addAndMakeVisible(clearButton);
    addAndMakeVisible(settingsButton);
    addChildComponent(model12RoutingButton);

    connectionStatusLabel.setJustificationType(juce::Justification::centred);
    connectionStatusLabel.setFont(juce::FontOptions(21.0f));
    connectionStatusLabel.setColour(juce::Label::textColourId,
                                    juce::Colour(paleText));
    connectionStatusLabel.setText(
        "Premi il pulsante per applicare il routing completo in una sola volta",
        juce::dontSendNotification);
    addChildComponent(connectionStatusLabel);

    recordButton.onClick = [this]
    {
        engine.toggleRecording(selectedMemory);
        updateControls();
    };
    clearButton.onClick = [this]
    {
        engine.clearMemory(selectedMemory);
        updateControls();
    };
    settingsButton.onClick = [this] { toggleSettings(); };
    model12RoutingButton.onClick = [this] { configureModel12Routing(); };

    decaySlider.setSliderStyle(juce::Slider::LinearHorizontal);
    decaySlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 34);
    decaySlider.setRange(0.80, 1.0, 0.001);
    decaySlider.setValue(engine.getAudioDecay(), juce::dontSendNotification);
    decaySlider.setColour(juce::Slider::trackColourId, memoryColours[4]);
    decaySlider.setColour(juce::Slider::thumbColourId, juce::Colours::white);
    decaySlider.onValueChange = [this]
    {
        engine.setAudioDecay(static_cast<float>(decaySlider.getValue()));
    };
    decayLabel.setText("MEMORIA DEL RESPIRO", juce::dontSendNotification);
    decayLabel.setColour(juce::Label::textColourId, juce::Colour(quietText));
    addAndMakeVisible(decaySlider);
    addAndMakeVisible(decayLabel);

    selectMemory(0);
    setSize(1280, 800);
    initialiseAudio();
    juce::Timer::startTimerHz(30);
    juce::HighResolutionTimer::startTimer(2);
}

MainComponent::~MainComponent()
{
    juce::HighResolutionTimer::stopTimer();
    juce::Timer::stopTimer();
    deviceManager.removeMidiInputDeviceCallback({}, this);
    deviceManager.removeAudioCallback(&engine);
    connectionViewport.setViewedComponent(nullptr, false);
    if (deviceSelector != nullptr)
        deviceSelector->setLookAndFeel(nullptr);
}

void MainComponent::initialiseAudio()
{
    // Start without opening the default input pair. On the Model 12 that pair
    // is USB 1+2, which is also used for Commento's stereo return and may form
    // an unwanted loop before the sax pair has been selected.
    const auto error = deviceManager.initialise(0, 2, nullptr, true);
    deviceSelector = std::make_unique<juce::AudioDeviceSelectorComponent>(
        // Audio routing is deliberately handled by configureModel12Routing().
        // The generic JUCE channel lists are omitted because applying one side
        // at a time can leave a multichannel ALSA device closed.
        deviceManager, 0, 0, 0, 0, true, true, false, false);
    deviceSelector->setItemHeight(70);
    deviceSelector->setLookAndFeel(&connectionLookAndFeel);
    connectionViewport.setViewedComponent(deviceSelector.get(), false);
    connectionViewport.setScrollBarsShown(true, false);
    connectionViewport.setScrollBarThickness(28);
    connectionViewport.setColour(juce::ScrollBar::thumbColourId,
                                 juce::Colour(0xff6a7c91));
    addChildComponent(connectionViewport);

    for (const auto& input : juce::MidiInput::getAvailableDevices())
        deviceManager.setMidiInputDeviceEnabled(input.identifier, true);
    deviceManager.addMidiInputDeviceCallback({}, this);
    deviceManager.addAudioCallback(&engine);

    statusLabel.setText(error.isEmpty() ? "audio pronto" : error,
                        juce::dontSendNotification);
    configureModel12Routing();
}

void MainComponent::configureModel12Routing()
{
    const auto findModel12 = [](const juce::StringArray& names)
    {
        for (const auto& name : names)
            if (name.containsIgnoreCase("model 12")
                || name.containsIgnoreCase("model12"))
                return name;
        for (const auto& name : names)
            if (name.containsIgnoreCase("tascam"))
                return name;
        return juce::String {};
    };

    juce::AudioIODeviceType* model12Type = nullptr;
    juce::String inputDeviceName;
    juce::String outputDeviceName;

    for (auto* type : deviceManager.getAvailableDeviceTypes())
    {
        type->scanForDevices();
        const auto outputName = findModel12(type->getDeviceNames(false));
        const auto inputName = type->hasSeparateInputsAndOutputs()
            ? findModel12(type->getDeviceNames(true)) : outputName;

        if (outputName.isNotEmpty() && inputName.isNotEmpty())
        {
            model12Type = type;
            inputDeviceName = inputName;
            outputDeviceName = outputName;
            break;
        }
    }

    if (model12Type == nullptr)
    {
        connectionStatusLabel.setColour(juce::Label::textColourId,
                                        juce::Colour(0xffff9f8e));
        connectionStatusLabel.setText(
            "MODEL 12 non trovata: controlla USB e alimentazione del mixer",
            juce::dontSendNotification);
        return;
    }

    deviceManager.setCurrentAudioDeviceType(model12Type->getTypeName(), true);
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.inputDeviceName = inputDeviceName;
    setup.outputDeviceName = outputDeviceName;
    setup.sampleRate = 48000.0;
    setup.bufferSize = 256;
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels.clear();
    setup.inputChannels.setBit(6);  // Model 12 USB input 7
    setup.inputChannels.setBit(7);  // Model 12 USB input 8
    setup.outputChannels.clear();
    setup.outputChannels.setBit(0); // Model 12 USB output 1
    setup.outputChannels.setBit(1); // Model 12 USB output 2

    const auto error = deviceManager.setAudioDeviceSetup(setup, true);
    if (error.isNotEmpty())
    {
        connectionStatusLabel.setColour(juce::Label::textColourId,
                                        juce::Colour(0xffff9f8e));
        connectionStatusLabel.setText("Errore ALSA: " + error,
                                      juce::dontSendNotification);
        return;
    }

    connectionStatusLabel.setColour(juce::Label::textColourId,
                                    juce::Colour(0xff91e5c4));
    connectionStatusLabel.setText(
        "MODEL 12 pronta: 48 kHz / 256  |  SAX 7+8  |  USCITA 1+2",
        juce::dontSendNotification);
}

void MainComponent::handleIncomingMidiMessage(juce::MidiInput*,
                                               const juce::MidiMessage& message)
{
    engine.enqueueMidiMessage(message);
    if (auto* output = deviceManager.getDefaultMidiOutput())
        output->sendMessageNow(message);
}

void MainComponent::timerCallback()
{
    animationPhase += 0.055;
    for (auto& orb : orbs)
    {
        orb->animation = animationPhase;
        orb->repaint();
    }

    updateControls();
    if (settingsVisible)
        enlargeConnectionTargets();
}

void MainComponent::enlargeConnectionTargets()
{
    if (deviceSelector == nullptr)
        return;

    const auto visit = [](auto&& self, juce::Component& parent) -> void
    {
        if (auto* list = dynamic_cast<juce::ListBox*>(&parent))
            list->setRowHeight(54);
        for (auto* child : parent.getChildren())
            self(self, *child);
    };
    visit(visit, *deviceSelector);
}

void MainComponent::hiResTimerCallback()
{
    auto loopOutput = engine.takeMidiOutput();
    if (! loopOutput.isEmpty())
        if (auto* output = deviceManager.getDefaultMidiOutput())
            output->sendBlockOfMessagesNow(loopOutput);
}

void MainComponent::selectMemory(int index)
{
    selectedMemory = juce::jlimit(0, EcosystemEngine::memoryCount - 1, index);
    for (int orbIndex = 0; orbIndex < EcosystemEngine::memoryCount; ++orbIndex)
        orbs[static_cast<size_t>(orbIndex)]->selected = orbIndex == selectedMemory;
    decaySlider.setVisible(selectedMemory == EcosystemEngine::midiMemoryCount
                           && ! settingsVisible);
    decayLabel.setVisible(decaySlider.isVisible());
    updateControls();
}

void MainComponent::updateControls()
{
    const auto recording = engine.isRecording(selectedMemory);
    recordButton.setButtonText(recording ? "CHIUDI LA MEMORIA" : "SEMINA");
    recordButton.setToggleState(recording, juce::dontSendNotification);
    clearButton.setEnabled(recording || engine.hasMaterial(selectedMemory));

    const auto type = selectedMemory < EcosystemEngine::midiMemoryCount
        ? "MIDI " + juce::String(engine.getMidiChannelForMemory(selectedMemory))
        : "SAX / AUDIO 7+8";
    const auto count = selectedMemory < EcosystemEngine::midiMemoryCount
        ? "  -  " + juce::String(engine.getEventCount(selectedMemory)) + " eventi" : "";
    statusLabel.setText(type + count, juce::dontSendNotification);
}

void MainComponent::toggleSettings()
{
    settingsVisible = ! settingsVisible;
    connectionViewport.setVisible(settingsVisible);
    model12RoutingButton.setVisible(settingsVisible);
    connectionStatusLabel.setVisible(settingsVisible);
    for (auto& orb : orbs)
        orb->setVisible(! settingsVisible);
    recordButton.setVisible(! settingsVisible);
    clearButton.setVisible(! settingsVisible);
    decaySlider.setVisible(! settingsVisible
        && selectedMemory == EcosystemEngine::midiMemoryCount);
    decayLabel.setVisible(decaySlider.isVisible());
    settingsButton.setButtonText(settingsVisible ? "TORNA ALLE MEMORIE" : "CONNESSIONI");
    resized();
    enlargeConnectionTargets();
}

void MainComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(background));
    juce::ColourGradient atmosphere(juce::Colour(0xff1a3141).withAlpha(0.36f),
                                    0.0f, 0.0f,
                                    juce::Colour(background).withAlpha(0.0f),
                                    static_cast<float>(getWidth()),
                                    static_cast<float>(getHeight()), false);
    graphics.setGradientFill(atmosphere);
    graphics.fillAll();

    if (settingsVisible)
    {
        graphics.setColour(juce::Colour(panel));
        graphics.fillRoundedRectangle(getLocalBounds().toFloat().reduced(42.0f)
                                      .withTrimmedTop(72.0f).withTrimmedBottom(88.0f), 18.0f);
    }
}

void MainComponent::resized()
{
    auto bounds = getLocalBounds().reduced(38);
    auto header = bounds.removeFromTop(72);
    titleLabel.setBounds(header.removeFromLeft(235));
    subtitleLabel.setBounds(header.removeFromLeft(330));
    statusLabel.setBounds(header);

    auto footer = bounds.removeFromBottom(92);
    settingsButton.setBounds(footer.removeFromLeft(190).reduced(4, 14));
    clearButton.setBounds(footer.removeFromRight(190).reduced(4, 14));
    recordButton.setBounds(footer.withSizeKeepingCentre(300, 64));

    if (settingsVisible)
    {
        auto connectionArea = bounds.reduced(24, 10);
        model12RoutingButton.setBounds(
            connectionArea.removeFromTop(82).reduced(4, 5));
        connectionStatusLabel.setBounds(connectionArea.removeFromTop(48));
        connectionArea.removeFromTop(8);
        connectionViewport.setBounds(connectionArea);
        if (deviceSelector != nullptr)
        {
            const auto contentWidth = juce::jmax(760,
                connectionViewport.getMaximumVisibleWidth() - 12);
            deviceSelector->setBounds(0, 0, contentWidth, 1080);
            enlargeConnectionTargets();
        }
        return;
    }

    const auto area = bounds.toFloat();
    const auto base = juce::jmin(area.getWidth() / 3.55f, area.getHeight() / 2.18f);
    const std::array<juce::Point<float>, EcosystemEngine::memoryCount> centres {
        juce::Point<float>(area.getX() + area.getWidth() * 0.17f, area.getY() + area.getHeight() * 0.29f),
        juce::Point<float>(area.getX() + area.getWidth() * 0.42f, area.getY() + area.getHeight() * 0.25f),
        juce::Point<float>(area.getX() + area.getWidth() * 0.29f, area.getY() + area.getHeight() * 0.70f),
        juce::Point<float>(area.getX() + area.getWidth() * 0.57f, area.getY() + area.getHeight() * 0.68f),
        juce::Point<float>(area.getX() + area.getWidth() * 0.79f, area.getY() + area.getHeight() * 0.46f)
    };

    for (int index = 0; index < EcosystemEngine::memoryCount; ++index)
    {
        const auto size = index == EcosystemEngine::midiMemoryCount ? base * 1.13f : base;
        orbs[static_cast<size_t>(index)]->setBounds(
            juce::Rectangle<float>(size, size).withCentre(centres[static_cast<size_t>(index)]).toNearestInt());
    }

    const auto sliderArea = juce::Rectangle<int>(
        static_cast<int>(area.getRight() - base * 1.3f),
        static_cast<int>(area.getBottom() - 58.0f),
        static_cast<int>(base * 1.18f), 42);
    decaySlider.setBounds(sliderArea);
    decayLabel.setBounds(sliderArea.translated(0, -30));
}
