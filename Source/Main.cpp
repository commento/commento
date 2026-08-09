#include <JuceHeader.h>
#include "MainComponent.h"

class CommentoApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "Commento"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise(const juce::String& commandLine) override
    {
        mainWindow = std::make_unique<MainWindow>(
            getApplicationName(), ! commandLine.contains("--windowed"));
    }

    void shutdown() override { mainWindow.reset(); }
    void systemRequestedQuit() override { quit(); }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow(const juce::String& name, bool kiosk)
            : DocumentWindow(name, juce::Colour(0xff090b12),
                             kiosk ? 0 : DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(! kiosk);
            setContentOwned(new MainComponent(), true);
            setResizable(! kiosk, ! kiosk);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);

           #if JUCE_LINUX
            if (kiosk)
                juce::Desktop::getInstance().setKioskModeComponent(this, false);
           #else
            juce::ignoreUnused(kiosk);
           #endif
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(CommentoApplication)

